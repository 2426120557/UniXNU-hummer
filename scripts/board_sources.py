"""Where this board's own sources live while a recipe runs.

Until v186 the board drivers were checked out at ``drivers/<name>`` in the build
tree, because the device repository had them at that path and the resolver
linked every device module in.  Format 2 keeps only the components named in
``[components] include``, and the hummer board drivers are not in the component
catalog, so they moved into the device ``package/``.  A package appears inside a
build tree as ``kits/<board.kit>``, which is the path every recipe sees while it
runs; a standalone run (the repository itself) sees ``package/drivers/<name>``.

``driver()`` answers with whichever of the two exists, so the same recipe works
in both places and the tree stays the only explanation of the build.
"""
import os
from pathlib import Path

# The kit name of this board ([board] kit in board.toml); package/ becomes kits/<kit>.
KIT = 'hummer'


def _roots():
    """Candidate base directories, nearest first: the tree, then the device repository."""
    roots = [Path(__file__).resolve().parents[1]]
    device = os.environ.get('UNIXNU_DEVICE')
    if device:
        roots.append(Path(device).resolve())
    return roots


def driver(name):
    """Directory of a board driver, from the running tree or the device package."""
    for base in _roots():
        for candidate in (base / 'kits' / KIT / 'drivers' / name, base / 'drivers' / name, base / 'package/drivers' / name):
            if candidate.is_dir():
                return candidate
    raise SystemExit('%s: board driver is missing; it belongs in %s/package/drivers/' % (name, _roots()[0]))


def kit_path(*parts):
    """A file of the board package, such as the GPU bring-up sources."""
    for base in _roots():
        for root in (base / 'kits' / KIT, base / 'package'):
            candidate = root.joinpath(*parts)
            if candidate.exists():
                return candidate
    return _roots()[0] / 'kits' / KIT / Path(*parts)


def tree():
    """The build tree, which is where a recipe runs from."""
    return _roots()[0]
