^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package libmiti
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.1.2 (2026-11-04)
------------------
* Builds with MSVC on Windows x64. ``file:///C:/dir/a.stl`` names ``C:\dir\a.stl``; a path whose
  root is not a drive, such as ``\\host\share``, is refused before any lookup, and a file is judged
  by its long name, never by an 8.3 short name.
* Asset URIs that contain a NUL character are refused, before any file is opened.
* An asset's extension is read from a path that still exists; MSVC builds refused every asset.

0.1.1 (2026-11-03)
------------------
* ``package://`` files that are symlinks, as ``colcon build --symlink-install`` installs them, are
  served. ``..`` still cannot lead out of a package, and ``file://`` roots stay strict.

0.1.0 (2026-11-02)
------------------
* First release: a WebSocket server for the MITI protocol (``miti.bridge.v1``), with a scheduler per
  subscription, JPEG and point cloud transforms, link statistics, and package and file assets.
