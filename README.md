# **Mini-VSFS (Very Simple File System)**

## **📌 Overview**

Mini-VSFS is a streamlined, user-space emulator for the **Very Simple File System (VSFS)** architecture (commonly conceptualized in operating systems literature like OSTEP). It implements a fully functional, block-based file system stored entirely within a simulated raw disk image file.

This implementation is intentionally constrained to demonstrate core OS concepts—such as inode management, block allocation, and binary serialization—without the overhead of complex modern file system features.

## **⚙️ Technical Specifications**

Unlike a standard VSFS implementation, Mini-VSFS cuts specific corners to maintain simplicity while enforcing strict data integrity via CRC32 checksums.

* **Block Size:** 4096 bytes (4 KiB)  
* **Inode Size:** 128 bytes  
* **Directory Entry (Dirent):** 64 bytes (Max filename length: 58 characters)  
* **Maximum File Size:** 49,152 bytes (48 KiB) — limited by 12 direct pointers per inode.  
* **Volume Size Limits:** 180 KiB minimum to 4096 KiB maximum (must be a multiple of 4). This maximum exists because the bitmap is exactly one 4096-byte block, limiting the system to 32,768 bits/blocks.  
* **Inode Limits:** 128 to 512 total inodes.

### **Key Architectural Differences from standard VSFS:**

1. **No Indirect Pointers:** Files are limited strictly to DIRECT\_MAX (12) blocks.  
2. **Flat Directory Structure:** Only the root directory (/) is supported. Subdirectories are not implemented.  
3. **Fixed Bitmaps:** The system allocates exactly one block (4096 bytes) for the Inode Bitmap and one block for the Data Bitmap, hard-capping total manageable resources.  
4. **Data Integrity:** Superblocks and Inodes utilize **CRC32 checksums**, while directory entries use an XOR checksum to prevent data corruption.

## **📂 Disk Layout**

The simulated disk image is partitioned linearly into 4096-byte blocks (where **N** represents the number of Inode blocks):

| Block 0 | Block 1 | Block 2 | Blocks 3 to (3 \+ N) | Blocks (3 \+ N \+ 1\) to End |
| :---- | :---- | :---- | :---- | :---- |
| **Superblock** | **Inode Bitmap** | **Data Bitmap** | **Inode Table** | **Data Region** |

* **Superblock (116 bytes):** Stores magic number (0x4D565346), block size, block counts, and region offsets.  
* **Inode Table:** Stores 128-byte inodes containing metadata (mode, size, timestamps) and direct block pointers.  
* **Data Region:** Block 0 of this region is strictly reserved for the root directory entries (. and ..).

## **🛠️ Build & Installation**

The toolchain is written in C17. Ensure you have GCC or Clang installed.

**Compile the image builder:**

gcc \-O2 \-std=c17 \-Wall \-Wextra mkfs\_builder.c \-o mkfs\_builder

**Compile the file adder:**

gcc \-O2 \-std=c17 \-Wall \-Wextra mkfs\_adder.c \-o mkfs\_adder

## **🚀 Usage**

The project provides two primary CLI utilities to interact with the file system.

### **1\. Formatting a New File System (mkfs\_builder)**

Creates a new .img file initialized with the Superblock, Bitmaps, Inode Table, and an empty Root Directory.

./mkfs\_builder \--image \<out.img\> \--size-kib \<180..4096\> \--inodes \<128..512\>

**Example:**

./mkfs\_builder \--image fs.img \--size-kib 1024 \--inodes 256

### **2\. Injecting Files into the File System (mkfs\_adder)**

Reads a file from your host operating system and writes it directly into the virtual Mini-VSFS image, updating inodes, allocating data blocks, and updating the root directory entries. **Prerequisite:** This tool requires a valid disk image already created by mkfs\_builder.

./mkfs\_adder \--input \<in.img\> \--output \<out.img\> \--file \<host\_filename\>

**Example:**

\# Adds 'hello.txt' to fs.img and saves the updated system to fs\_updated.img  
./mkfs\_adder \--input fs.img \--output fs\_updated.img \--file hello.txt

## **💡 Engineering Implementation Notes**

* **State Persistence:** Metadata and binary structs are packed using \#pragma pack(push, 1\) to prevent compiler padding from corrupting on-disk byte alignments.  
* **Block Allocation:** Uses bitwise operations to scan the 4096-byte bitmap arrays to locate free data blocks and inodes in ![][image1] time.  
* **Checksum Verification:** Every modification to the filesystem recalculates standard CRC32 hashes over the metadata block before writing to disk, ensuring structural integrity during emulator mounts.

[image1]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADMAAAAaCAYAAAAaAmTUAAAEcElEQVR4XrVWTUgVURSeQYOiol+TfO/NndGFZEGREAi2y8hFLSwoKNq0sEUQtChwJUhYEBEiCCKERAQaRYTUooUQVGSboJ+VoBBEixACoQKt79yfmfs3b55aHxxm7vnOOfeec8+9M0HgQVhN4ZA1wOfj09UM5bymIEGxfy5vVcRj56pC22ulKHApoFNwuyLjIr4Iq/avzTGO4wN41Nt6QhahSiyLYoydRMxel9FgMNogiqI2OL+Nk/htpVI5jPf1JFHEziPwImQys87Q0tKyK2JsBv5dNgf/rZAb8L0P+QX5E1gJQ3cBMkMc5DfszyiOYhPnxrbys4dw+pnEcX97e/s6k+KJdoFfCmghpiP5DcBvwuenkCTJfhaxd7BdQKH22Xxra+tm8M+oiDaHuc/B702pVNpB4/xt4gj5lsJpEIM6myU0NDRsgs20vRDoEsgsqnlM19uAzdmIRYN4DsO23+YxdzP0T/DcJjTZksvlcknOke5YLlC1Q1SxUqmJZy7g5o9g44no4UDxtEjIHE2YGrquAYuiESz0CGNRB3bgCxVB59Gm3dCN8oHrH8L3LviHeK13aQksYgOMnjLRyy40T0oGi7qm6eppAjGJ/+ATqNqweYQ2Kcv5/kAu6TYYX6fC6Dp90Zi7F/wXiqGpTVB7oFrLZGhzHDIi7NbDZirSkmlqatoJ3UdaSOZgIZTnhbF7FINUMpk3zc3NW2jc2Ni4EeNJsrNcU6CInbBZwPydPp4DBkMyOG2hUDpWfEGNsJllafVC3ucYf0UxrpjWJsgH0qeNP0OW1TmjOIlxXlyAa0d70o3aY3Mc/AZh7KVM5oqZhfkFFv3OlvULgE+AKxvP45qpUwzY3CJ/oQ+DOIn75ZwPAtGq3eDvmF4mYLMbyczzzpAwpuEGjM1TYGdBJuj6HSY7/frNTUaDfl6UjgoC3QKJfBfnxdMRAvy25WvNTUadA1oky9s+VA4fzRGZ8FVZXI7CrQ/cFlOg7wkTH9HfaNNXWYv5M4LdHsh3ughsjoPcQF6UyQzbPBCCPw1uCTKa7oqcj3mqZYF2dJS3GB9lRLlc2QD9czn3kD+FDLILfkC6ha3Hg24UGLyALJpMWIcAl5n4vbipEslChOoWIt8xqdJBiRyEfEKcLn1HFRD3jEymx7M0w5xaGXbfIXs0tRUz5N+a7Qg8ARmXE/RB5iCvEaTNMjfembgNX9JlovS0E9DRdU8LVfJe/Y4oxOKGnE4XqIJ7MmPiXKXXOcFjlgFn4zgWciqJk6M0sb1wH+TCv9m/OSnyHIOqlAH1KxUn7m9QMfSeKJhRTZQ4/1sFjisA4ndAZlG4vZn238U3gB2lP+oP+F2vZFrzO+XCw3pUgfgOjUEGgjyLmpHrbjRhiLNGH8LbmlKDFSQ3poTGo31P4OqeaqGzssIwAjVZmUZ020W5X3F/QKMc2rsCXQyQx0mSsIy3dtznWB16qH+FolT+G/yTOVpHsUKsseQStTuuaY9W4SJgOq46TD6KJnA1KapQq8Vfd7n7OQYJBncAAAAASUVORK5CYII=>