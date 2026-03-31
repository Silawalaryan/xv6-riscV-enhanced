#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static void
usage(void)
{
  fprintf(2, "usage: mv SRC DST\n");
  exit(1);
}

static char*
basename(char *s)
{
  char *p = s;
  for(char *q = s; *q; q++)
    if(*q == '/')
      p = q + 1;
  return p;
}

static int
is_dir(char *path)
{
  struct stat st;
  if(stat(path, &st) < 0)
    return 0;
  return st.type == T_DIR;
}

static void
resolve_dst(char *src, char *dst, char *out, int outsz)
{
  if(is_dir(dst)){
    char *base = basename(src);
    int dl = strlen(dst);
    int bl = strlen(base);
    int need = dl + 1 + bl + 1;
    if(need > outsz){
      fprintf(2, "mv: path too long\n");
      exit(1);
    }
    memmove(out, dst, dl);
    if(dl > 0 && dst[dl-1] != '/')
      out[dl++] = '/';
    memmove(out + dl, base, bl);
    out[dl + bl] = 0;
  } else {
    if(strlen(dst) + 1 > outsz){
      fprintf(2, "mv: path too long\n");
      exit(1);
    }
    strcpy(out, dst);
  }
}

static int
copy_file(char *src, char *dst)
{
  int sfd = open(src, O_RDONLY);
  if(sfd < 0)
    return -1;

  int dfd = open(dst, O_CREATE | O_WRONLY | O_TRUNC);
  if(dfd < 0){
    close(sfd);
    return -1;
  }

  char buf[512];
  int n;
  int err = 0;
  while((n = read(sfd, buf, sizeof(buf))) > 0){
    int off = 0;
    while(off < n){
      int m = write(dfd, buf + off, n - off);
      if(m < 0){
        err = 1;
        break;
      }
      off += m;
    }
    if(err)
      break;
  }
  if(n < 0)
    err = 1;

  close(sfd);
  close(dfd);
  if(err)
    return -1;
  return 0;
}

int
main(int argc, char *argv[])
{
  if(argc != 3)
    usage();

  char *src = argv[1];
  char *dst = argv[2];

  char dstpath[128];
  memset(dstpath, 0, sizeof(dstpath));
  resolve_dst(src, dst, dstpath, sizeof(dstpath));

  // Fast path: link+unlink acts like rename on xv6 (same filesystem).
  if(link(src, dstpath) == 0){
    if(unlink(src) < 0){
      fprintf(2, "mv: cannot remove %s\n", src);
      exit(1);
    }
    exit(0);
  }

  // Fallback: copy then unlink.
  if(copy_file(src, dstpath) < 0){
    fprintf(2, "mv: cannot move %s to %s\n", src, dstpath);
    exit(1);
  }

  if(unlink(src) < 0){
    fprintf(2, "mv: copied but cannot remove %s\n", src);
    exit(1);
  }

  exit(0);
}