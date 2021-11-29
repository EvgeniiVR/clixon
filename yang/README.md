# Yang files

There are two classes of Yang files
  * Clixon yang files.
  * Mandatory: "Standard" yang files necessary for clixon lib/client/backend to run

Both are always installed. If you want to change where the are
installed, configure with: `--with-yang-installdir=DIR`

The third (optional) is only installed if configure flag
`--enable-optyang` is set. Further, the optional yang files are
installed in `--with-opt-yang-installdir=DIR` if given, otherwise in
the same dir as the mandatory.