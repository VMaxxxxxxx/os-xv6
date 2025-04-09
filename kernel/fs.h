// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define FSMAGIC 0x10203040

// #define NDIRECT 12
// #define NINDIRECT (BSIZE / sizeof(uint))
// #define MAXFILE (NDIRECT + NINDIRECT)

// // On-disk inode structure
// struct dinode {
//   short type;           // File type
//   short major;          // Major device number (T_DEVICE only)
//   short minor;          // Minor device number (T_DEVICE only)
//   short nlink;          // Number of links to inode in file system
//   uint size;            // Size of file (bytes)
//   uint addrs[NDIRECT+1];   // Data block addresses
// };
// 修改 inode 的结构，改成 11 个直接块、 1 个一级间接块、 1 个二级间接块
#define NDIRECT 11  // 直接块
#define NINDIRECT (BSIZE / sizeof(uint))  // 一级间接块数量
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT) // 二级间接块数量

// inode structure
struct dinode
{
  short type;   // 文件类型
  short major;  // 主设备号
  short minor;  // 次设备号
  short nlink;  // inode的连接数
  uint size;    // 文件大小
  uint addrs[NDIRECT + 2]; // 0 - 10 直接索引  11： 一级间接索引  12：二级间接索引
};
// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

