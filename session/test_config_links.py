"""Tests for config_links.py. Run: python3 -m pytest session/ (needs pytest and hypothesis)."""
import os

from hypothesis import given, strategies as st

import config_links as cl

NAMES = st.sampled_from(["fish", "kitty", "kwinrc", "kdeglobals", "vesktop", "frametop", ".keep", cl.REPLACED])
KINDS = st.sampled_from([cl.OURS, cl.OTHER, cl.REAL])
OWN = "frametop"


def apply_plan(real, local, actions):
    """The folder's entries after the actions, as plan() means them."""
    local = dict(local)
    for action, name in actions:
        if action == cl.LINK:
            assert name not in local
            local[name] = cl.OURS
        elif action == cl.REPLACE:
            assert local[name] == cl.REAL
            local[name] = cl.OURS
        elif action == cl.UNLINK:
            assert local[name] == cl.OURS
            del local[name]
    return local


@given(st.sets(NAMES), st.dictionaries(NAMES, KINDS), st.sets(NAMES))
def test_skipped_names_untouched(real, local, keep):
    for _, name in cl.plan(real, local, keep, OWN):
        assert name not in keep and name not in (OWN, cl.REPLACED)


@given(st.sets(NAMES), st.dictionaries(NAMES, KINDS), st.sets(NAMES))
def test_real_entries_reachable(real, local, keep):
    after = apply_plan(real, local, cl.plan(real, local, keep, OWN))
    for name in real - keep - {OWN, cl.REPLACED}:
        assert after[name] in (cl.OURS, cl.OTHER)


@given(st.sets(NAMES), st.dictionaries(NAMES, KINDS), st.sets(NAMES))
def test_only_our_links_removed(real, local, keep):
    after = apply_plan(real, local, cl.plan(real, local, keep, OWN))
    for name, kind in local.items():
        if kind != cl.OURS or name in keep:
            assert name in after
        if kind == cl.OTHER:
            assert after[name] == cl.OTHER


@given(st.sets(NAMES), st.dictionaries(NAMES, KINDS), st.sets(NAMES))
def test_no_dangling_links(real, local, keep):
    after = apply_plan(real, local, cl.plan(real, local, keep, OWN))
    for name, kind in after.items():
        if kind == cl.OURS and name not in keep | {OWN, cl.REPLACED}:
            assert name in real


@given(st.sets(NAMES), st.dictionaries(NAMES, KINDS), st.sets(NAMES))
def test_idempotent(real, local, keep):
    after = apply_plan(real, local, cl.plan(real, local, keep, OWN))
    assert cl.plan(real, after, keep, OWN) == []


def test_sync_on_disk(tmp_path):
    real = tmp_path / ".config"
    local = real / OWN
    (real / "fish").mkdir(parents=True)
    (real / "fish" / "config.fish").write_text("real")
    (real / "kwinrc").write_text("stock desktop")
    (real / "starship.toml").write_text("prompt")
    local.mkdir()
    (local / "fish").mkdir()
    (local / "fish" / "config.fish").write_text("stub")
    (local / "kwinrc").write_text("frametop")
    (local / "gone").symlink_to(os.path.join("..", "gone"))
    (local / cl.REPLACED / "fish").mkdir(parents=True)  # an earlier move's

    moved = cl.sync(str(real), str(local), {"kwinrc"}, cl.SHARE)

    assert (local / "fish" / "config.fish").read_text() == "real"
    assert os.readlink(local / "starship.toml") == os.path.join("..", "starship.toml")
    assert (local / "kwinrc").read_text() == "frametop"
    assert not os.path.lexists(local / "gone")
    assert not os.path.lexists(local / OWN)
    assert (local / cl.REPLACED / "fish.1" / "config.fish").read_text() == "stub"
    assert moved == [("fish", str(local / cl.REPLACED / "fish.1"))]
    assert cl.sync(str(real), str(local), {"kwinrc"}, cl.SHARE) == []


@given(st.dictionaries(NAMES, KINDS), st.sets(NAMES))
def test_unshare_removes_only_ours(local, keep):
    after = apply_plan(set(), local, cl.plan(set(), local, keep, OWN))
    for name, kind in local.items():
        if kind != cl.OURS or name in keep | {OWN, cl.REPLACED}:
            assert after[name] == kind
        else:
            assert name not in after


def test_unshare_on_disk(tmp_path):
    real = tmp_path / ".config"
    local = real / OWN
    (real / "fish").mkdir(parents=True)
    local.mkdir()
    (local / "kwinrc").write_text("frametop")
    (local / "elsewhere").symlink_to("/tmp")

    cl.sync(str(real), str(local), {"kwinrc"}, cl.SHARE)
    assert os.path.islink(local / "fish")

    assert cl.sync(str(real), str(local), {"kwinrc"}, cl.UNSHARE) == []
    assert sorted(os.listdir(local)) == ["elsewhere", "kwinrc"]
