use std::collections::BTreeSet;

use super::{
    validate_key, BPlusTree, PageKind, Result, StoredKey, StoredValue, LEAF_CELL_HEADER_LEN,
    MAX_INLINE_KEY_BYTES, MAX_TREE_KEY_BYTES, MAX_TREE_VALUE_BYTES, PAGE_BODY_CAPACITY, SLOT_LEN,
};
use crate::{corruption, BtreeError};

pub(super) const OVERFLOW_CELL_CAPACITY: usize = PAGE_BODY_CAPACITY - SLOT_LEN;

impl BPlusTree {
    pub(super) fn store_key(&mut self, key: &[u8]) -> Result<StoredKey> {
        validate_key(key)?;
        if key.len() <= MAX_INLINE_KEY_BYTES {
            return Ok(StoredKey::Inline(key.to_vec()));
        }
        let first_page_id = self.commit_overflow_blob(key)?;
        Ok(StoredKey::Overflow {
            bytes: key.to_vec(),
            first_page_id,
        })
    }

    pub(super) fn load_key_blob(
        &mut self,
        first_page_id: u64,
        encoded_len: u16,
        seen: Option<&mut BTreeSet<u64>>,
    ) -> Result<StoredKey> {
        let bytes = self.read_overflow_blob(
            first_page_id,
            u32::from(encoded_len),
            seen,
            MAX_TREE_KEY_BYTES,
            "key",
        )?;
        Ok(StoredKey::Overflow {
            bytes,
            first_page_id,
        })
    }

    pub(super) fn store_value(&mut self, key: &StoredKey, value: &[u8]) -> Result<StoredValue> {
        if value.len() > MAX_TREE_VALUE_BYTES {
            return Err(BtreeError::InvalidInput(format!(
                "B+ tree value has {} bytes; maximum is {MAX_TREE_VALUE_BYTES}",
                value.len()
            )));
        }

        let inline_occupied = SLOT_LEN
            .checked_add(LEAF_CELL_HEADER_LEN)
            .and_then(|length| length.checked_add(key.encoded_payload_len()))
            .and_then(|length| length.checked_add(value.len()))
            .ok_or_else(|| {
                BtreeError::InvalidInput("inline leaf value extent overflowed usize".to_owned())
            })?;
        if inline_occupied <= PAGE_BODY_CAPACITY {
            return Ok(StoredValue::Inline(value.to_vec()));
        }

        let first_page_id = self.commit_overflow_blob(value)?;
        Ok(StoredValue::Overflow {
            len: u32::try_from(value.len()).map_err(|_| {
                BtreeError::InvalidInput("overflow value length does not fit u32".to_owned())
            })?,
            first_page_id,
        })
    }

    pub(super) fn load_value(&mut self, value: &StoredValue) -> Result<Vec<u8>> {
        match value {
            StoredValue::Inline(bytes) => Ok(bytes.clone()),
            StoredValue::Overflow { len, first_page_id } => {
                self.read_overflow_blob(*first_page_id, *len, None, MAX_TREE_VALUE_BYTES, "value")
            }
        }
    }

    pub(super) fn validate_value_reachability(
        &mut self,
        value: &StoredValue,
        seen: &mut BTreeSet<u64>,
    ) -> Result<()> {
        if let StoredValue::Overflow { len, first_page_id } = value {
            self.read_overflow_blob(
                *first_page_id,
                *len,
                Some(seen),
                MAX_TREE_VALUE_BYTES,
                "value",
            )?;
        }
        Ok(())
    }

    fn commit_overflow_blob(&mut self, value: &[u8]) -> Result<u64> {
        if value.is_empty() {
            return Err(BtreeError::InvalidInput(
                "empty values must remain inline instead of using overflow pages".to_owned(),
            ));
        }
        let mut next = None;
        for chunk in value.rchunks(OVERFLOW_CELL_CAPACITY) {
            let (mut page, recycled) = self.prepare_tree_page(PageKind::Overflow)?;
            page.insert_cell(chunk)?;
            page.set_overflow_next(next)?;
            next = Some(self.commit_tree_page(page, recycled)?);
        }
        next.ok_or_else(|| corruption(0, "overflow value produced no pages"))
    }

    fn read_overflow_blob(
        &mut self,
        first_page_id: u64,
        encoded_len: u32,
        mut global_seen: Option<&mut BTreeSet<u64>>,
        max_len: usize,
        blob_kind: &str,
    ) -> Result<Vec<u8>> {
        let expected_len = usize::try_from(encoded_len)
            .map_err(|_| corruption(0, "overflow value length does not fit usize"))?;
        if expected_len == 0 || expected_len > max_len {
            return Err(corruption(
                0,
                format!("overflow {blob_kind} encodes invalid length {expected_len}"),
            ));
        }
        let page_count = expected_len
            .checked_add(OVERFLOW_CELL_CAPACITY - 1)
            .ok_or_else(|| corruption(0, "overflow page-count calculation overflowed usize"))?
            / OVERFLOW_CELL_CAPACITY;
        let first_chunk_len = expected_len
            .checked_sub((page_count - 1) * OVERFLOW_CELL_CAPACITY)
            .ok_or_else(|| corruption(0, "overflow first-chunk length underflowed usize"))?;

        let mut current = Some(first_page_id);
        let mut local_seen = BTreeSet::new();
        let mut output = Vec::with_capacity(expected_len);
        for index in 0..page_count {
            let page_id = current.ok_or_else(|| {
                corruption(
                    0,
                    format!("overflow chain ended after {index} of {page_count} pages"),
                )
            })?;
            if !local_seen.insert(page_id) {
                return Err(corruption(
                    0,
                    format!("overflow chain contains cycle at page {page_id}"),
                ));
            }
            if let Some(seen) = global_seen.as_deref_mut() {
                if !seen.insert(page_id) {
                    return Err(corruption(
                        0,
                        format!("overflow page {page_id} is referenced more than once"),
                    ));
                }
            }

            let page = self.pager.read_page(page_id)?;
            if page.kind() != PageKind::Overflow {
                return Err(corruption(
                    0,
                    format!("overflow chain page {page_id} has kind {:?}", page.kind()),
                ));
            }
            if page.cell_count() != 1 {
                return Err(corruption(
                    0,
                    format!(
                        "overflow page {page_id} has {} cells; expected exactly one",
                        page.cell_count()
                    ),
                ));
            }
            let chunk = page.cell(0)?;
            let expected_chunk_len = if index == 0 {
                first_chunk_len
            } else {
                OVERFLOW_CELL_CAPACITY
            };
            if chunk.len() != expected_chunk_len {
                return Err(corruption(
                    0,
                    format!(
                        "overflow page {page_id} stores {} bytes; canonical chain position {index} requires {expected_chunk_len}",
                        chunk.len()
                    ),
                ));
            }
            output.extend_from_slice(chunk);
            current = page.overflow_next();
        }
        if let Some(extra) = current {
            return Err(corruption(
                0,
                format!("overflow chain continues to unexpected extra page {extra}"),
            ));
        }
        if output.len() != expected_len {
            return Err(corruption(
                0,
                format!(
                    "overflow chain reconstructed {} bytes; expected {expected_len}",
                    output.len()
                ),
            ));
        }
        Ok(output)
    }
}

#[cfg(test)]
mod tests {
    use tempfile::tempdir;

    use super::BPlusTree;
    use crate::tree::MAX_TREE_VALUE_BYTES;

    #[test]
    fn one_mebibyte_value_round_trips_reopen_and_delete() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("overflow-value.db");
        let mut tree = BPlusTree::create_new(&path, 16).expect("create tree");
        let value = (0..MAX_TREE_VALUE_BYTES)
            .map(|index| ((index * 131 + 17) & 0xff) as u8)
            .collect::<Vec<_>>();

        assert_eq!(
            tree.put(b"large", &value).expect("insert large value"),
            None
        );
        assert_eq!(
            tree.get(b"large").expect("read large value"),
            Some(value.clone())
        );
        let pages_after_insert = tree.data_page_count();
        assert!(pages_after_insert > 200);
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 8).expect("reopen tree");
        assert_eq!(
            reopened.get(b"large").expect("reopened read"),
            Some(value.clone())
        );
        assert_eq!(
            reopened.delete(b"large").expect("delete large value"),
            Some(value)
        );
        assert_eq!(reopened.root_page_id(), None);
    }

    #[test]
    fn replacing_large_value_reuses_old_overflow_pages_on_next_mutation() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("overflow-reuse.db");
        let mut tree = BPlusTree::create_new(&path, 8).expect("create tree");
        let first = vec![0x11; MAX_TREE_VALUE_BYTES];
        let second = vec![0x22; MAX_TREE_VALUE_BYTES];
        let third = vec![0x33; MAX_TREE_VALUE_BYTES];

        tree.put(b"key", &first).expect("first large value");
        tree.put(b"key", &second).expect("second large value");
        let peak_pages = tree.data_page_count();
        tree.put(b"key", &third).expect("third large value");
        assert_eq!(tree.data_page_count(), peak_pages);
        assert_eq!(tree.get(b"key").expect("latest value"), Some(third));
    }

    #[test]
    fn boundary_inline_value_stays_page_local() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("inline-boundary.db");
        let mut tree = BPlusTree::create_new(&path, 4).expect("create tree");
        let key = b"key";
        let inline_len =
            super::PAGE_BODY_CAPACITY - super::SLOT_LEN - super::LEAF_CELL_HEADER_LEN - key.len();
        let value = vec![0x5a; inline_len];

        tree.put(key, &value).expect("insert maximum inline value");
        assert_eq!(tree.data_page_count(), 1);
        assert_eq!(tree.get(key).expect("read inline boundary"), Some(value));
    }
}
