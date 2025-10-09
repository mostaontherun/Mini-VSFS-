# Mini-VSFS-
MiniVSFS, based on VSFS, is fairly simple – a block-based file system structure with a superblock, inode and data bitmaps, inode tables, and data blocks. 
Compared to the regular VSFS, MiniVSFS cuts a few corners:

●	Indirect pointer mechanism is not implemented.
●	Only supported directory is the root (/) directory.
●	Only one block each for the inode and data bitmap.
●	Limited size and inode counts.
