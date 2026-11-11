# Updates

**Settings > Updates** checks whether a newer Volta OS has been published on
[GitHub](https://github.com/Omardigio/VoltaOS/releases) and installs it with one click.

1. Opening the page looks for updates by itself (**Check again** repeats it).
2. If there is a new version you see its number, the download size and **What's new**.
3. **Install update** downloads the new AppImage. The bar shows the progress and **Cancel** stops it.
4. When it finishes, **Restart now** closes Volta OS and opens the new version.

Your settings, API keys and sign-ins live outside the AppImage, so they are kept.

## How it stays safe

- The new file is downloaded **next to** the current one with another name. Before replacing anything
  it checks that the size matches what GitHub announces, that the SHA-256 matches (when GitHub
  provides it) and that it really is an AppImage.
- The replacement is a single atomic rename: if the download is interrupted, the PC loses power or the
  file is damaged, the version you have keeps working untouched.
- It needs about 250 MB of free space in the folder where the AppImage is.

## When it cannot install by itself

Only an **AppImage** can replace itself. If you run Volta OS from source (`start.sh`, a build folder) the
page still tells you a new version exists and offers **Open releases page**; to update, pull the
repository and build again. The same happens if the AppImage sits in a folder you cannot write to:
move it somewhere in your home folder.

## For whoever publishes a release

The page compares the version compiled into the app (`project(VoltaOS VERSION ...)` in
`CMakeLists.txt`) with the tag of the **latest** release (`v1.1`, `v1.2.0`...; drafts and
pre-releases are ignored). To make an update appear:

1. raise `VERSION` in `CMakeLists.txt` and build the AppImage;
2. publish a release whose tag is that version, with the file `VoltaOS-<version>-x86_64.AppImage`
   attached (the file name must contain `x86_64` and end in `.AppImage`).
