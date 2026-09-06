use std::collections::BTreeSet;

use super::{BPlusTree, Page, PageKind, Result};
use crate::{corruption, SUPERBLOCK_COUNT};

impl BPlusTree {
    pub(super) fn refresh_reusable_pages(&mut self) -> Result<()> {
        let mut reachable = BTreeSet::new();
        if let Some(root) = self.pager.root_page_id() {
            self.validate_subtree(root, 0, &mut reachable)?;
        }
        let end = SUPERBLOCK_COUNT
            .checked_add(self.pager.data_page_count())
            .ok_or_else(|| {
                corruption(0, "committed page range overflowed u64 during reuse scan")
            })?;
        self.reusable_pages.clear();
        self.reusable_pages
            .extend((SUPERBLOCK_COUNT..end).filter(|page_id| !reachable.contains(page_id)));
        Ok(())
    }

    pub(super) fn prepare_tree_page(&mut self, kind: PageKind) -> Result<(Page, bool)> {
        if let Some(page_id) = self.reusable_pages.pop_front() {
            return Ok((Page::new(page_id, kind)?, true));
        }
        Ok((self.pager.prepare_new_page(kind)?, false))
    }

    pub(super) fn commit_tree_page(&mut self, page: Page, recycled: bool) -> Result<u64> {
        if !recycled {
            return self.pager.commit_new_page(page);
        }

        self.pager.commit_recycled_page(page)
    }

    #[cfg(test)]
    pub(super) fn reusable_page_ids_for_test(&mut self) -> Result<Vec<u64>> {
        self.refresh_reusable_pages()?;
        Ok(self.reusable_pages.iter().copied().collect())
    }
}

#[cfg(test)]
mod tests {
    use tempfile::tempdir;

    use super::BPlusTree;
    use crate::tree::{StoredKey, StoredValue};
    use crate::PageKind;

    #[test]
    fn repeated_updates_recycle_orphan_pages_instead_of_growing_forever() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("reuse-updates.db");
        let mut tree = BPlusTree::create_new(&path, 4).expect("create tree");

        tree.put(b"key", b"v1").expect("first value");
        assert_eq!(tree.data_page_count(), 1);
        tree.put(b"key", b"v2").expect("second value");
        assert_eq!(tree.data_page_count(), 2);
        tree.put(b"key", b"v3").expect("third value");
        assert_eq!(tree.data_page_count(), 2);
        tree.put(b"key", b"v4").expect("fourth value");
        assert_eq!(tree.data_page_count(), 2);
        assert_eq!(
            tree.get(b"key").expect("latest value"),
            Some(b"v4".to_vec())
        );
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 2).expect("reopen tree");
        assert_eq!(reopened.data_page_count(), 2);
        assert_eq!(
            reopened.get(b"key").expect("reopened latest value"),
            Some(b"v4".to_vec())
        );
    }

    #[test]
    fn recycled_shadow_write_cannot_change_current_root_before_publication() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("reuse-shadow.db");
        let mut tree = BPlusTree::create_new(&path, 4).expect("create tree");
        tree.put(b"key", b"old").expect("initial value");
        tree.put(b"key", b"current").expect("current value");
        let committed_root = tree.root_page_id();

        let reusable = tree
            .reusable_page_ids_for_test()
            .expect("derive reusable pages");
        assert_eq!(reusable.len(), 1);
        let (mut shadow, recycled) = tree
            .prepare_tree_page(PageKind::Leaf)
            .expect("prepare recycled shadow");
        assert!(recycled);
        let cell = super::super::encode_leaf_cell(&super::super::LeafEntry {
            key: StoredKey::Inline(b"key".to_vec()),
            value: StoredValue::Inline(b"unpublished".to_vec()),
        })
        .expect("encode shadow value");
        shadow.insert_cell(&cell).expect("pack recycled shadow");
        tree.commit_tree_page(shadow, true)
            .expect("write recycled shadow");
        assert_eq!(tree.root_page_id(), committed_root);
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 2).expect("reopen tree");
        assert_eq!(reopened.root_page_id(), committed_root);
        assert_eq!(
            reopened.get(b"key").expect("authoritative value"),
            Some(b"current".to_vec())
        );
    }

    #[test]
    fn pages_from_empty_tree_are_reused_by_later_insert() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("reuse-empty.db");
        let mut tree = BPlusTree::create_new(&path, 2).expect("create tree");
        tree.put(b"key", b"value").expect("insert value");
        assert_eq!(tree.data_page_count(), 1);
        tree.delete(b"key").expect("delete final key");
        assert_eq!(tree.root_page_id(), None);
        let pages = tree.data_page_count();

        tree.put(b"other", b"replacement")
            .expect("reuse orphan page");
        assert_eq!(tree.data_page_count(), pages);
        assert_eq!(
            tree.get(b"other").expect("replacement value"),
            Some(b"replacement".to_vec())
        );
    }
}
