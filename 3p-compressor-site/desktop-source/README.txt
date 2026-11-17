3P Compressor
=============
Build (Windows, MinGW-w64 / GCC 9+):   build.bat          -> 3P-Compressor.exe (GUI) + p3cli.exe
Build (Visual Studio):                 build_msvc.bat

Using the GUI
  * Drag a file onto the window, double-click a .3p file, or press Browse.
  * Any file that is not .3p  -> button says "Compress" -> creates <name>.3p next to it.
  * A .3p file                -> button says "Extract"  -> restores the ORIGINAL name + extension (stored in the file).
  * While running the button says "Cancel" (Esc also cancels). Existing files are never overwritten: "name (1).ext".

Audio (WAV): 8/16/24-bit PCM files are detected automatically. Each sample is predicted from the previous ones
  (adaptive stereo-aware filter) and only the small difference is compressed. The encoder tries 3 filter speeds
  plus "plain" per 8 MB segment and keeps the smallest. Audio runs at about 0.5 MB/s per CPU core (extract = same).

Duplicates (any distance): before compressing, the file is scanned for long repeats anywhere in it (a rolling-hash
  index, bounded memory) and each repeat is replaced by a tiny copy record. Repeating runs (zeros, patterns) are
  single records. Extraction rebuilds them at memory speed. Not used for WAV/BMP files (their modelling needs the exact layout).

Headerless data: a statistics pass plus a short trial decides whether a file without a header is 16-bit PCM audio or
  has a fixed record / image-row size, and switches the matching model on only if it is clearly smaller.

Tables, series, images (headerless): the encoder can replace 1/2/4-byte words by their difference to the previous record
  (tables / time series) or by a gradient prediction (images), optionally zigzag-coded, when a short trial shows it is smaller.
  WAV audio residuals are zigzag-coded too.

Generated data: files that are the raw output of a Mersenne Twister (Python random.randbytes, numpy RandomState.bytes) are
  stored as 2.5 KB (the generator state) and regenerated exactly on extract. Every byte is verified when compressing.

Programs (x86 / ARM64 / ARM32 executables, DLLs, .so): relative CALL/JMP/BL targets are rewritten to absolute addresses so
  identical calls become identical bytes (reversible by construction). Chosen only if a trial on the most code-like part of
  the file is clearly smaller. Combines with the duplicate finder.

JPEG (baseline / extended-sequential Huffman): the picture is decoded to its quantised DCT coefficients, which are coded
  with a context-mixing model that looks at the neighbouring blocks; extraction rebuilds the original file bit for bit.
  The compressor re-creates the file and compares before accepting, so progressive, damaged or unusual JPEGs simply use the
  general model. EXIF/ICC data and anything appended after the image is kept and compressed normally.

How a file is handled (automatic, nothing to configure):
  0. baseline JPEG                                -> coefficient recompression (about 17-25% smaller)
  1. whole file is a Mersenne Twister stream      -> store the generator state (2.5 KB)
  2. WAV / BMP header                             -> exact audio / pixel model
  3. long repeats anywhere in the file            -> copy records (+ program filter if it is code)
  4. headerless: PCM audio, tables, series, image rows, program code -> matching transform, only if a trial wins
  5. everything else                              -> general context-mixing model (text, data, already-compressed data)
  Every transform is exactly reversible, every segment carries a checksum, and the encoder never accepts a method that
  is not smaller than plain coding on a sample.

Command line (p3cli.exe)
  p3cli c <file> [outdir] [-t threads] [-v]     compress  (-v = verify every segment while compressing)
  p3cli x <file.3p> [outdir] [-t threads]       extract
  p3cli i <file.3p>                              show stored name / original size

Files:  engine3p.hpp  the engine (portable C++17, header-only)     gui_win32.cpp  the window
