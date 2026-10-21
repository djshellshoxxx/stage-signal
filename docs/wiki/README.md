# Wiki source

These pages are the source for the StageSignal GitHub wiki. The wiki itself is a separate git repository (`djshellshoxxx/stage-signal.wiki`), so the pages live here until they are published.

## Publish

GitHub only creates the wiki repository after the first page is saved in the web UI.

1. On GitHub, open the repository's **Wiki** tab and create the **Home** page (any text). This creates the wiki repository.
2. Clone it next to this checkout:

   ```sh
   # Clone the wiki repository
   git clone https://github.com/djshellshoxxx/stage-signal.wiki.git ../stage-signal.wiki
   ```

3. Copy the pages in, then commit and push:

   ```sh
   # Copy the wiki pages into the wiki repository and push them
   cp docs/wiki/*.md ../stage-signal.wiki/
   git -C ../stage-signal.wiki add -A
   git -C ../stage-signal.wiki commit -m "Publish wiki pages from docs/wiki"
   git -C ../stage-signal.wiki push
   ```

Do not copy this README into the wiki. Page names are flat, with hyphens, and `_Sidebar.md` and `_Footer.md` are special to GitHub.

When a page changes in code, change it here in the same PR, then publish again.
