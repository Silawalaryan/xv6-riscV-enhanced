#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static void
usage(void)
{
  fprintf(2, "usage: cp SRC DST\n");
  exit(1);
}

// Return pointer to last path element in s.
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

// If dst is a directory, fill out with dst + "/" + basename(src).
// Otherwise copy dst into out.
static void
resolve_dst(char *src, char *dst, char *out, int outsz)
{
  if(is_dir(dst)){
    char *base = basename(src);
    int dl = strlen(dst);
    int bl = strlen(base);
    int need = dl + 1 + bl + 1;
    if(need > outsz){
      fprintf(2, "cp: path too long\n");
      exit(1);
    }
    memmove(out, dst, dl);
    if(dl > 0 && dst[dl-1] != '/')
      out[dl++] = '/';
    memmove(out + dl, base, bl);
    out[dl + bl] = 0;
  } else {
    if(strlen(dst) + 1 > outsz){
      fprintf(2, "cp: path too long\n");
      exit(1);
    }
    strcpy(out, dst);
  }
}

int
main(int argc, char *argv[])
{
  if(argc != 3)
    usage();

  char *src = argv[1];
  char *dst = argv[2];

  int sfd = open(src, O_RDONLY);
  if(sfd < 0){
    fprintf(2, "cp: cannot open %s\n", src);
    exit(1);
  }

  char dstpath[128];
  memset(dstpath, 0, sizeof(dstpath));
  resolve_dst(src, dst, dstpath, sizeof(dstpath));

  int dfd = open(dstpath, O_CREATE | O_WRONLY | O_TRUNC);
  if(dfd < 0){
    fprintf(2, "cp: cannot create %s\n", dstpath);
    close(sfd);
    exit(1);
  }

  char buf[512];
  int n;
  while((n = read(sfd, buf, sizeof(buf))) > 0){
    int off = 0;
    while(off < n){
      int m = write(dfd, buf + off, n - off);
      if(m < 0){
        fprintf(2, "cp: write error\n");
        close(sfd);
        close(dfd);
        exit(1);
      }
      off += m;
    }
  }
  if(n < 0)
    fprintf(2, "cp: read error\n");

  close(sfd);
  close(dfd);
  exit(n < 0);
}