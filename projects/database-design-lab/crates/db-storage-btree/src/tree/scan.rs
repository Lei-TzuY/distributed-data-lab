use super::{validate_key, BPlusTree, MAX_TREE_HEIGHT};
use crate::{corruption, PageKind, Result};

impl BPlusTree {
    /// Returns up to `limit` live key/value pairs in ascending bytewise key order from `[start, end)`.
    ///
    /// `end = None` means no upper bound. The point-tree format intentionally keeps leaf sibling
    /// pointers at zero; scans therefore walk internal children in key order. Exact child minimum
    /// separators let traversal skip subtrees that end before `start`, while `end` and `limit` stop
    /// the walk without introducing any additional persistent edge that copy-on-write mutation would
    /// need to maintain.
    pub fn range_scan(
        &mut self,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let before = self.pager.read_page_calls();
        let result = self.range_scan_uninstrumented(start, end, limit);
        if let Ok(rows) = &result {
            self.instrumentation.range_scans = self.instrumentation.range_scans.saturating_add(1);
            self.instrumentation.range_page_accesses = self
                .instrumentation
                .range_page_accesses
                .saturating_add(self.pager.read_page_calls().saturating_sub(before));
            self.instrumentation.range_result_records = self
                .instrumentation
                .range_result_records
                .saturating_add(u64::try_from(rows.len()).unwrap_or(u64::MAX));
        }
        result
    }

    pub(super) fn range_scan_uninstrumented(
        &mut self,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        validate_key(start)?;
        if let Some(end) = end {
            validate_key(end)?;
            if end < start {
                return Err(crate::BtreeError::InvalidInput(
                    "ordered range end must not sort before start".to_owned(),
                ));
            }
        }
        if limit == 0 || end.is_some_and(|end| end == start) {
            return Ok(Vec::new());
        }
        let Some(root) = self.pager.root_page_id() else {
            return Ok(Vec::new());
        };

        let mut rows = Vec::new();
        self.scan_subtree(root, start, end, limit, 0, &mut rows)?;
        Ok(rows)
    }

    fn scan_subtree(
        &mut self,
        page_id: u64,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
        depth: usize,
        rows: &mut Vec<(Vec<u8>, Vec<u8>)>,
    ) -> Result<bool> {
        if depth >= MAX_TREE_HEIGHT {
            return Err(corruption(
                0,
                format!("B+ tree range scan exceeded maximum height {MAX_TREE_HEIGHT}"),
            ));
        }
        let page = self.pager.read_page(page_id)?;
        match page.kind() {
            PageKind::Leaf => {
                let entries = self.decode_leaf(&page, None)?;
                for entry in entries {
                    let key = entry.key.as_slice();
                    if key < start {
                        continue;
                    }
                    if end.is_some_and(|end| key >= end) {
                        return Ok(true);
                    }
                    let value = self.load_value(&entry.value)?;
                    rows.push((key.to_vec(), value));
                    if rows.len() >= limit {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            PageKind::Internal => {
                let children = self.decode_internal(&page, None)?;
                for (index, child) in children.iter().enumerate() {
                    if children
                        .get(index + 1)
                        .is_some_and(|next| next.min_key.as_slice() <= start)
                    {
                        continue;
                    }
                    if end.is_some_and(|end| child.min_key.as_slice() >= end) {
                        return Ok(true);
                    }
                    if self.scan_subtree(child.page_id, start, end, limit, depth + 1, rows)? {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            PageKind::Overflow => Err(corruption(
                0,
                format!("range traversal reached overflow page {page_id} as a tree node"),
            )),
        }
    }
}

#[cfg(test)]
mod tests {
    use tempfile::tempdir;

    use super::BPlusTree;

    fn key(index: u16) -> Vec<u8> {
        index.to_be_bytes().to_vec()
    }

    #[test]
    fn half_open_ranges_are_sorted_limited_and_read_only() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("scan.db");
        let mut tree = BPlusTree::create_new(&path, 8).expect("create tree");
        for index in 0..96_u16 {
            tree.put(&key(index), &[(index & 0xff) as u8; 128])
                .expect("insert row");
        }
        assert!(tree.height().expect("height") >= 2);
        let generation = tree.generation();
        let page_count = tree.data_page_count();

        let rows = tree
            .range_scan(&key(17), Some(&key(44)), 9)
            .expect("bounded range");
        assert_eq!(rows.len(), 9);
        assert_eq!(rows.first().expect("first").0, key(17));
        assert_eq!(rows.last().expect("last").0, key(25));
        assert!(rows.windows(2).all(|pair| pair[0].0 < pair[1].0));
        assert_eq!(tree.generation(), generation);
        assert_eq!(tree.data_page_count(), page_count);

        assert!(tree
            .range_scan(&key(44), Some(&key(44)), 100)
            .expect("empty equal-bound range")
            .is_empty());
        assert!(tree
            .range_scan(&key(0), None, 0)
            .expect("zero-limit range")
            .is_empty());
        assert!(tree.range_scan(&key(50), Some(&key(49)), 1).is_err());
    }

    #[test]
    fn range_scan_survives_reopen_delete_and_overflow_values() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("scan-reopen.db");
        let mut tree = BPlusTree::create_new(&path, 4).expect("create tree");
        let large = vec![0x5a; 32 * 1024];
        for index in 0..40_u16 {
            let value = if index == 21 {
                large.clone()
            } else {
                vec![(index & 0xff) as u8; 192]
            };
            tree.put(&key(index), &value).expect("insert row");
        }
        tree.delete(&key(20)).expect("delete predecessor");
        tree.delete(&key(22)).expect("delete successor");
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 4).expect("reopen tree");
        let rows = reopened
            .range_scan(&key(19), Some(&key(24)), 16)
            .expect("scan reopened tree");
        let keys = rows.iter().map(|row| row.0.clone()).collect::<Vec<_>>();
        assert_eq!(keys, vec![key(19), key(21), key(23)]);
        assert_eq!(rows[1].1, large);
    }
}
