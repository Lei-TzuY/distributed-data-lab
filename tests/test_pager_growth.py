import glob
import os
import shutil
import subprocess
import tempfile


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, 'build', 'tinydb_pager_growth_probe'),
        os.path.join(repo_root, 'build', 'Debug', 'tinydb_pager_growth_probe.exe'),
        os.path.join(repo_root, 'build', 'Release', 'tinydb_pager_growth_probe.exe'),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError('tinydb_pager_growth_probe executable was not built')

    temp_dir = tempfile.mkdtemp(prefix='tinydb-pager-growth-')
    db_path = os.path.join(temp_dir, 'growth.db')
    try:
        result = subprocess.run(
            [probe, db_path],
            capture_output=True,
            text=True,
            timeout=180,
        )
        assert result.returncode == 0, result.stdout + '\n' + result.stderr
        assert 'PAGER_GROWTH_OK' in result.stdout, result.stdout
        assert 'legacy_ceiling=4096' in result.stdout, result.stdout
        assert 'pager_publish_pages=64' in result.stdout, result.stdout
        assert 'buffer_pool=16' in result.stdout, result.stdout
        assert 'eviction_safe=yes' in result.stdout, result.stdout
        assert 'publish_rollback=yes' in result.stdout, result.stdout
        assert 'preexisting_dirty_rollback=yes' in result.stdout, result.stdout
        assert 'pin_eviction_guard=yes' in result.stdout, result.stdout
        assert 'pinned_rwlock=yes' in result.stdout, result.stdout
        assert 'legacy_lock_pin=yes' in result.stdout, result.stdout
        assert 'unowned_unpin_safe=yes' in result.stdout, result.stdout
        assert 'destructive_pin_guard=yes' in result.stdout, result.stdout

        # The probe writes pages 0..4128 inclusive, proving that the old
        # TABLE_MAX_PAGES boundary is no longer a Pager allocation limit. It
        # proves both explicit pinned handles and source-compatible legacy
        # page-number read/write locks remain frame-stable across LRU churn,
        # verifies ordinary get_page cleanup cannot steal their owned pins, and
        # proves free/shrink/transaction rollback fail closed while a target
        # frame is pinned. Once the real owner releases the pin, normal eviction
        # and rollback behavior resumes. The probe also publishes and rolls back
        # 64 staged page images through a 16-frame pool. One target is already
        # dirty and spilled before publication, so rollback must restore both
        # its older transactional image and dirty state across a second eviction
        # rather than resurrecting staged bytes.
        expected_pages = 4096 + 32 + 1
        assert os.path.getsize(db_path) == expected_pages * 4096
    finally:
        for path in glob.glob(db_path + '*'):
            try:
                os.remove(path)
            except OSError:
                pass
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
