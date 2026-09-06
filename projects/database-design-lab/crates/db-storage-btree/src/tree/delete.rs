use super::{
    cells_fit, choose_split, encode_internal_cell, encode_leaf_cell, route_child,
    validate_child_refs, validate_key, BPlusTree, ChildRef, Page, PageKind, Result,
    MAX_TREE_HEIGHT, PAGE_BODY_CAPACITY, SLOT_LEN,
};
use crate::{corruption, BtreeError};

const MIN_PAGE_FILL: usize = PAGE_BODY_CAPACITY / 2;

impl BPlusTree {
    /// Deletes one binary key and returns its previous value when present.
    ///
    /// Deletion follows the same copy-on-write publication rule as insertion: reachable pages are
    /// never overwritten. The changed leaf and ancestors are appended first, underfull children are
    /// merged or byte-balanced with an adjacent sibling, and only then does one mirrored-superblock
    /// transition publish the replacement root. A root with one remaining internal child contracts
    /// to that child; deleting the final key publishes an empty tree.
    pub fn delete(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        let data_write_before = self.pager.data_page_bytes_written();
        let previous = self.get_uninstrumented(key)?;
        let Some(previous_value) = previous else {
            self.record_mutation(key.len(), data_write_before);
            return Ok(None);
        };
        self.refresh_reusable_pages()?;
        let root = self.pager.root_page_id().ok_or_else(|| {
            corruption(
                0,
                "B+ tree lookup found a value while committed root is empty",
            )
        })?;

        let replacement = self.rewrite_delete(root, key, 0)?;
        let new_root = match replacement {
            None => None,
            Some(mut root_ref) => {
                loop {
                    let page = self.pager.read_page(root_ref.page_id)?;
                    if page.kind() != PageKind::Internal {
                        break;
                    }
                    let children = self.decode_internal(&page, None)?;
                    if children.len() != 1 {
                        break;
                    }
                    root_ref = children[0].clone();
                }
                Some(root_ref.page_id)
            }
        };
        self.pager.set_root(new_root)?;
        self.record_mutation(key.len(), data_write_before);
        Ok(Some(previous_value))
    }

    fn rewrite_delete(
        &mut self,
        page_id: u64,
        key: &[u8],
        depth: usize,
    ) -> Result<Option<ChildRef>> {
        if depth >= MAX_TREE_HEIGHT {
            return Err(corruption(
                0,
                format!("B+ tree delete exceeded maximum height {MAX_TREE_HEIGHT}"),
            ));
        }

        let page = self.pager.read_page(page_id)?;
        match page.kind() {
            PageKind::Leaf => {
                let mut entries = self.decode_leaf(&page, None)?;
                let index = entries
                    .binary_search_by(|entry| entry.key.as_slice().cmp(key))
                    .map_err(|_| {
                        corruption(0, "delete rewrite could not find previously located key")
                    })?;
                entries.remove(index);
                if entries.is_empty() {
                    Ok(None)
                } else {
                    self.commit_leaf(&entries).map(Some)
                }
            }
            PageKind::Internal => {
                let mut children = self.decode_internal(&page, None)?;
                let child_index = route_child(&children, key)?;
                let child_id = children[child_index].page_id;
                match self.rewrite_delete(child_id, key, depth + 1)? {
                    Some(replacement) => children[child_index] = replacement,
                    None => {
                        children.remove(child_index);
                    }
                }

                if children.is_empty() {
                    return Ok(None);
                }
                let affected = child_index.min(children.len() - 1);
                self.rebalance_after_delete(&mut children, affected)?;
                if children.is_empty() {
                    Ok(None)
                } else {
                    self.commit_internal(&children).map(Some)
                }
            }
            PageKind::Overflow => Err(corruption(
                0,
                format!("delete traversal reached overflow page {page_id} as a tree node"),
            )),
        }
    }

    fn rebalance_after_delete(
        &mut self,
        children: &mut Vec<ChildRef>,
        affected: usize,
    ) -> Result<()> {
        if children.len() < 2 {
            return Ok(());
        }
        validate_child_refs(children)?;
        let affected = affected.min(children.len() - 1);
        let affected_page = self.pager.read_page(children[affected].page_id)?;
        if !page_is_underfull(&affected_page)? {
            return Ok(());
        }

        let (left_index, right_index) = if affected > 0 {
            (affected - 1, affected)
        } else {
            (0, 1)
        };
        let left_page = self.pager.read_page(children[left_index].page_id)?;
        let right_page = self.pager.read_page(children[right_index].page_id)?;
        if left_page.kind() != right_page.kind() {
            return Err(corruption(
                0,
                format!(
                    "delete rebalance found sibling kind mismatch between pages {} and {}",
                    left_page.page_id(),
                    right_page.page_id()
                ),
            ));
        }

        let replacements = match left_page.kind() {
            PageKind::Leaf => self.rebalance_leaf_pair(&left_page, &right_page)?,
            PageKind::Internal => self.rebalance_internal_pair(&left_page, &right_page)?,
            PageKind::Overflow => {
                return Err(corruption(0, "overflow page appeared as a B+ tree sibling"));
            }
        };
        children.splice(left_index..=right_index, replacements);
        validate_child_refs(children)?;
        Ok(())
    }

    fn rebalance_leaf_pair(&mut self, left: &Page, right: &Page) -> Result<Vec<ChildRef>> {
        let mut entries = self.decode_leaf(left, None)?;
        entries.extend(self.decode_leaf(right, None)?);
        for pair in entries.windows(2) {
            if pair[0].key.as_slice() >= pair[1].key.as_slice() {
                return Err(corruption(
                    0,
                    "delete leaf rebalance encountered overlapping sibling key ranges",
                ));
            }
        }
        let cells = entries
            .iter()
            .map(encode_leaf_cell)
            .collect::<Result<Vec<_>>>()?;
        if cells_fit(&cells) {
            return Ok(vec![self.commit_leaf(&entries)?]);
        }

        let split = choose_split(&cells).ok_or_else(|| {
            BtreeError::InvalidInput(
                "delete leaf redistribution cannot divide sibling entries into two pages"
                    .to_owned(),
            )
        })?;
        Ok(vec![
            self.commit_leaf(&entries[..split])?,
            self.commit_leaf(&entries[split..])?,
        ])
    }

    fn rebalance_internal_pair(&mut self, left: &Page, right: &Page) -> Result<Vec<ChildRef>> {
        let mut grandchildren = self.decode_internal(left, None)?;
        grandchildren.extend(self.decode_internal(right, None)?);
        validate_child_refs(&grandchildren)?;
        let cells = grandchildren
            .iter()
            .map(encode_internal_cell)
            .collect::<Result<Vec<_>>>()?;
        if cells_fit(&cells) {
            return Ok(vec![self.commit_internal(&grandchildren)?]);
        }

        let split = choose_internal_rebalance_split(&cells).ok_or_else(|| {
            BtreeError::InvalidInput(
                "delete internal redistribution cannot keep two children on each sibling"
                    .to_owned(),
            )
        })?;
        Ok(vec![
            self.commit_internal(&grandchildren[..split])?,
            self.commit_internal(&grandchildren[split..])?,
        ])
    }
}

fn page_is_underfull(page: &Page) -> Result<bool> {
    if page.kind() == PageKind::Overflow {
        return Err(corruption(0, "overflow page appeared as a B+ tree child"));
    }
    if page.kind() == PageKind::Internal && page.cell_count() < 2 {
        return Ok(true);
    }
    let used = (0..page.cell_count()).try_fold(0_usize, |used, index| {
        let cell = page.cell(index)?;
        used.checked_add(SLOT_LEN)
            .and_then(|value| value.checked_add(cell.len()))
            .ok_or_else(|| corruption(0, "page occupancy overflowed usize during delete"))
    })?;
    Ok(used < MIN_PAGE_FILL)
}

fn choose_internal_rebalance_split(cells: &[Vec<u8>]) -> Option<usize> {
    if cells.len() < 4 {
        return None;
    }
    let sizes = cells
        .iter()
        .map(|cell| SLOT_LEN.checked_add(cell.len()))
        .collect::<Option<Vec<_>>>()?;
    let total = sizes
        .iter()
        .try_fold(0_usize, |sum, size| sum.checked_add(*size))?;
    let mut prefix = 0_usize;
    let mut best: Option<(usize, usize)> = None;
    for split in 1..cells.len() {
        prefix = prefix.checked_add(sizes[split - 1])?;
        if split < 2 || cells.len() - split < 2 {
            continue;
        }
        let right = total.checked_sub(prefix)?;
        if prefix <= PAGE_BODY_CAPACITY && right <= PAGE_BODY_CAPACITY {
            let imbalance = prefix.abs_diff(right);
            if best.is_none_or(|(_, current)| imbalance < current) {
                best = Some((split, imbalance));
            }
        }
    }
    best.map(|(split, _)| split)
}

#[cfg(test)]
mod tests {
    use tempfile::tempdir;

    use super::BPlusTree;

    #[test]
    fn missing_delete_is_a_true_noop() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("missing-delete.db");
        let mut tree = BPlusTree::create_new(&path, 2).expect("create tree");
        tree.put(b"present", b"value").expect("insert value");
        let generation = tree.generation();
        let pages = tree.data_page_count();
        let root = tree.root_page_id();

        assert_eq!(tree.delete(b"missing").expect("delete missing key"), None);
        assert_eq!(tree.generation(), generation);
        assert_eq!(tree.data_page_count(), pages);
        assert_eq!(tree.root_page_id(), root);
    }

    #[test]
    fn deleting_final_key_publishes_empty_tree_and_reopens() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("empty-after-delete.db");
        let mut tree = BPlusTree::create_new(&path, 2).expect("create tree");
        tree.put(&[0xff, 0x00], b"value").expect("insert value");

        assert_eq!(
            tree.delete(&[0xff, 0x00]).expect("delete value"),
            Some(b"value".to_vec())
        );
        assert_eq!(tree.root_page_id(), None);
        assert_eq!(tree.height().expect("empty height"), 0);
        assert_eq!(tree.get(&[0xff, 0x00]).expect("lookup deleted"), None);
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 2).expect("reopen empty tree");
        assert_eq!(reopened.root_page_id(), None);
        assert_eq!(reopened.height().expect("reopened empty height"), 0);
    }

    #[test]
    fn deletion_merges_redistributes_and_contracts_multilevel_root() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("delete-rebalance.db");
        let mut tree = BPlusTree::create_new(&path, 3).expect("create tree");
        let mut keys = Vec::new();

        for number in 0_u16..20 {
            let mut key = vec![b'k'; 510];
            key.extend_from_slice(&number.to_be_bytes());
            let value = vec![(number % 251) as u8; 900];
            tree.put(&key, &value).expect("insert split workload");
            keys.push((key, value));
        }
        let original_height = tree.height().expect("height before delete");
        assert!(original_height >= 3);

        for (key, value) in keys.iter().take(19) {
            assert_eq!(
                tree.delete(key).expect("delete from multilevel tree"),
                Some(value.clone())
            );
            assert_eq!(tree.get(key).expect("lookup deleted key"), None);
        }

        assert_eq!(tree.height().expect("contracted height"), 1);
        assert_eq!(
            tree.get(&keys[19].0).expect("lookup survivor"),
            Some(keys[19].1.clone())
        );
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 2).expect("reopen contracted tree");
        assert_eq!(reopened.height().expect("reopened height"), 1);
        assert_eq!(
            reopened.get(&keys[19].0).expect("lookup reopened survivor"),
            Some(keys[19].1.clone())
        );
        for (key, _) in keys.iter().take(19) {
            assert_eq!(reopened.get(key).expect("reopened deleted lookup"), None);
        }
    }
}
