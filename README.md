# Recycle Bin Browser

A small native Windows application for browsing the Recycle Bin on any local or removable drive.

It reads only the selected drive's `$RECYCLE.BIN` folder; it does not scan the entire drive and it does not alter Recycle Bin contents.

## Features

- Select any connected drive from the drop-down with the mouse or keyboard
- Automatically reload when a different drive is selected
- Search file names, original paths, and current Recycle Bin paths as you type
- Switch the application UI between English and Korean without restarting
- Restore a selected item to its recorded original location, with confirmation and no overwrite
- Select multiple files for batch restore or backup copying
- Sort by any column by clicking its heading
- Export the currently filtered result set as UTF-8 CSV
- Show the original file name, original path, size, deletion time, and current Recycle Bin path
- Open a selected payload file or reveal it in File Explorer
- Read Windows `$I` metadata correctly for both legacy and Windows 8+ Recycle Bin formats

## What the two locations mean

| Column | Meaning |
| --- | --- |
| **Original location (before deletion)** | Where the file lived before it was deleted, recorded in its `$I...` metadata file. |
| **Actual location (now)** | The file's current physical path in `$RECYCLE.BIN`, where Windows renames it to `$R...`. |

Windows preserves the original name and location in a metadata companion named `$I...`, while the file data itself is stored under a matching `$R...` name. A missing `$I...` file means the original name and path cannot be recovered from the Recycle Bin metadata.

## Build

The project uses only the Win32 API and Common Controls. With a MinGW-w64 GCC toolchain:

```powershell
gcc -std=c11 -O2 -mwindows -municode -static -finput-charset=UTF-8 -fwide-exec-charset=UTF-16LE RecycleBinBrowser.c -o RecycleBinBrowser.exe -lcomctl32 -lshell32 -lole32
```

## Safety

The application never overwrites an existing file at the target path. Restoring an item moves its `$R...` payload out of the Recycle Bin and removes its matching `$I...` metadata. Access to protected Recycle Bin folders or target locations may require running Windows as an administrator.

## License

MIT. See [LICENSE](LICENSE).
