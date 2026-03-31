// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"

// Command history.
// Stores the last 100 commands (without trailing newline).
#define HISTORY_SIZE 100
#define HISTORY_CMDLEN 128

char history[HISTORY_SIZE][HISTORY_CMDLEN];
int history_count;

static int
history_len(void)
{
  if(history_count < HISTORY_SIZE)
    return history_count;
  return HISTORY_SIZE;
}

static void
history_copy(char *dst, const char *src)
{
  int i;
  for(i = 0; i < HISTORY_CMDLEN - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

void
history_add(char *cmd)
{
  int i;
  // Skip leading spaces/tabs.
  while(*cmd == ' ' || *cmd == '\t')
    cmd++;
  if(*cmd == 0)
    return;

  // Copy into a temp buffer, stripping trailing \n/\r.
  char tmp[HISTORY_CMDLEN];
  memset(tmp, 0, sizeof(tmp));
  for(i = 0; i < HISTORY_CMDLEN - 1 && cmd[i] && cmd[i] != '\n' && cmd[i] != '\r'; i++)
    tmp[i] = cmd[i];
  tmp[i] = 0;
  if(tmp[0] == 0)
    return;

  int idx = history_count % HISTORY_SIZE;
  history_copy(history[idx], tmp);
  history_count++;
}

char*
history_get(int offset)
{
  int n = history_len();
  if(offset < 0 || offset >= n)
    return 0;
  // offset 0 is the most recent command.
  int base = history_count - 1 - offset;
  int idx = base % HISTORY_SIZE;
  if(idx < 0)
    idx += HISTORY_SIZE;
  return history[idx];
}

static void
history_print(void)
{
  int n = history_len();
  // Print oldest .. newest with 1-based indices.
  int start = history_count - n;
  if(start < 0)
    start = 0;
  for(int i = 0; i < n; i++){
    int idx = (start + i) % HISTORY_SIZE;
    printf("%d %s\n", i + 1, history[idx]);
  }
}

static void
history_save(void)
{
  int fd = open("/history", O_WRONLY | O_CREATE | O_TRUNC);
  if(fd < 0)
    return;

  int n = history_len();
  int start = history_count - n;
  if(start < 0)
    start = 0;

  for(int i = 0; i < n; i++){
    int idx = (start + i) % HISTORY_SIZE;
    int l = strlen(history[idx]);
    if(l > 0)
      write(fd, history[idx], l);
    write(fd, "\n", 1);
  }
  close(fd);
}

static void
history_load(void)
{
  int fd = open("/history", O_RDONLY);
  if(fd < 0)
    return;

  char line[HISTORY_CMDLEN];
  int len = 0;
  memset(line, 0, sizeof(line));

  for(;;){
    char c;
    int cc = read(fd, &c, 1);
    if(cc < 1)
      break;
    if(c == '\r')
      continue;
    if(c == '\n'){
      line[len] = 0;
      if(len > 0)
        history_add(line);
      len = 0;
      memset(line, 0, sizeof(line));
      continue;
    }
    if(len < HISTORY_CMDLEN - 1){
      line[len++] = c;
    }
  }
  if(len > 0){
    line[len] = 0;
    history_add(line);
  }
  close(fd);
}

static void
line_clear(int n)
{
  for(int i = 0; i < n; i++)
    write(2, "\b \b", 3);
}

static void
line_set(char *buf, int nbuf, int *lenp, const char *s)
{
  line_clear(*lenp);
  *lenp = 0;
  if(s == 0)
    return;
  int l = strlen(s);
  if(l > nbuf - 2)
    l = nbuf - 2;
  for(int i = 0; i < l; i++)
    buf[i] = s[i];
  *lenp = l;
  buf[*lenp] = 0;
  if(*lenp > 0)
    write(2, buf, *lenp);
}

static int
is_space(char c)
{
  return c == ' ' || c == '\t';
}

static int
starts_with(const char *s, const char *pre)
{
  int i;
  for(i = 0; pre[i]; i++){
    if(s[i] != pre[i])
      return 0;
  }
  return 1;
}

static int
common_prefix_len(const char *a, const char *b)
{
  int i = 0;
  while(a[i] && b[i] && a[i] == b[i])
    i++;
  return i;
}

// Return index of token start (0..len). Token is separated by whitespace.
static int
token_start(char *buf, int len)
{
  int i = len;
  while(i > 0 && !is_space(buf[i-1]))
    i--;
  return i;
}

// Fill dirpath and prefix based on token (may include '/').
// dirprefix is the part including trailing '/', used for reconstruction.
static void
split_path(const char *token, char *dirpath, char *dirprefix, char *prefix)
{
  int lastslash = -1;
  for(int i = 0; token[i]; i++){
    if(token[i] == '/')
      lastslash = i;
  }

  if(lastslash < 0){
    // No slash: directory is "." and prefix is token.
    dirpath[0] = '.';
    dirpath[1] = 0;
    dirprefix[0] = 0;
    history_copy(prefix, token);
    return;
  }

  if(lastslash == 0){
    // "/xxx": directory is "/".
    dirpath[0] = '/';
    dirpath[1] = 0;
    dirprefix[0] = '/';
    dirprefix[1] = 0;
  } else {
    // "a/b": directory is "a" and prefix is "b".
    int i;
    for(i = 0; i < HISTORY_CMDLEN - 1 && i < lastslash; i++)
      dirpath[i] = token[i];
    dirpath[i] = 0;

    // include trailing slash in dirprefix
    for(i = 0; i < HISTORY_CMDLEN - 1 && i <= lastslash; i++)
      dirprefix[i] = token[i];
    dirprefix[i] = 0;
  }

  // prefix after last slash
  int j = 0;
  for(int i = lastslash + 1; token[i] && j < HISTORY_CMDLEN - 1; i++)
    prefix[j++] = token[i];
  prefix[j] = 0;
}

static void
tab_complete(char *buf, int nbuf, int *lenp)
{
  int len = *lenp;
  int ts = token_start(buf, len);

  char token[HISTORY_CMDLEN];
  int tlen = len - ts;
  if(tlen < 0)
    return;
  if(tlen > HISTORY_CMDLEN - 1)
    tlen = HISTORY_CMDLEN - 1;
  for(int i = 0; i < tlen; i++)
    token[i] = buf[ts + i];
  token[tlen] = 0;

  char dirpath[HISTORY_CMDLEN];
  char dirprefix[HISTORY_CMDLEN];
  char prefix[HISTORY_CMDLEN];
  memset(dirpath, 0, sizeof(dirpath));
  memset(dirprefix, 0, sizeof(dirprefix));
  memset(prefix, 0, sizeof(prefix));
  split_path(token, dirpath, dirprefix, prefix);

  int fd = open(dirpath, O_RDONLY);
  if(fd < 0)
    return;

  struct dirent de;
  int nmatches = 0;
  char common[HISTORY_CMDLEN];
  char first[HISTORY_CMDLEN];
  memset(common, 0, sizeof(common));
  memset(first, 0, sizeof(first));

  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;

    // dirent names may not be NUL-terminated.
    char name[DIRSIZ+1];
    memset(name, 0, sizeof(name));
    for(int i = 0; i < DIRSIZ; i++)
      name[i] = de.name[i];
    name[DIRSIZ] = 0;

    // Trim at first NUL if present.
    for(int i = 0; i < DIRSIZ; i++){
      if(name[i] == 0)
        break;
      // keep going
    }

    if(!starts_with(name, prefix))
      continue;

    if(nmatches == 0){
      history_copy(first, name);
      history_copy(common, name);
    } else {
      int cpl = common_prefix_len(common, name);
      common[cpl] = 0;
    }
    nmatches++;
  }
  close(fd);

  if(nmatches == 0)
    return;

  // If there is exactly one match, complete to it. If the token is already
  // fully typed, append a '/' for directories or a trailing space for files
  // so Tab has a visible effect.
  if(nmatches == 1){
    char completed[HISTORY_CMDLEN];
    memset(completed, 0, sizeof(completed));
    int k = 0;
    for(int i = 0; dirprefix[i] && k < HISTORY_CMDLEN - 1; i++)
      completed[k++] = dirprefix[i];
    for(int i = 0; first[i] && k < HISTORY_CMDLEN - 1; i++)
      completed[k++] = first[i];
    completed[k] = 0;

    struct stat st;
    int isdir = 0;
    if(stat(completed, &st) >= 0 && st.type == T_DIR)
      isdir = 1;

    if(isdir){
      if(k > 0 && completed[k-1] != '/' && k < HISTORY_CMDLEN - 1)
        completed[k++] = '/';
      completed[k] = 0;
    }

    // If token already equals completed, add suffix.
    if(!isdir && strcmp(token, completed) == 0){
      if(k < HISTORY_CMDLEN - 1)
        completed[k++] = ' ';
      completed[k] = 0;
    }

    // Rebuild the whole input line.
    char newline[HISTORY_CMDLEN];
    memset(newline, 0, sizeof(newline));
    int outlen = 0;
    for(int i = 0; i < ts && outlen < HISTORY_CMDLEN - 1; i++)
      newline[outlen++] = buf[i];
    for(int i = 0; completed[i] && outlen < HISTORY_CMDLEN - 1; i++)
      newline[outlen++] = completed[i];
    newline[outlen] = 0;

    line_set(buf, nbuf, lenp, newline);
    return;
  }

  // If we can extend the prefix to 'common', do it.
  int prelen = strlen(prefix);
  int comlen = strlen(common);

  if(comlen > prelen){
    // Build new token = dirprefix + common
    char newtok[HISTORY_CMDLEN];
    memset(newtok, 0, sizeof(newtok));
    int k = 0;
    for(int i = 0; dirprefix[i] && k < HISTORY_CMDLEN - 1; i++)
      newtok[k++] = dirprefix[i];
    for(int i = 0; common[i] && k < HISTORY_CMDLEN - 1; i++)
      newtok[k++] = common[i];
    newtok[k] = 0;

    // Rebuild the whole input line.
    char newline[HISTORY_CMDLEN];
    memset(newline, 0, sizeof(newline));

    int outlen = 0;
    for(int i = 0; i < ts && outlen < HISTORY_CMDLEN - 1; i++)
      newline[outlen++] = buf[i];
    for(int i = 0; newtok[i] && outlen < HISTORY_CMDLEN - 1; i++)
      newline[outlen++] = newtok[i];
    newline[outlen] = 0;

    line_set(buf, nbuf, lenp, newline);
    return;
  }

  // Ambiguous: print matches and reprint prompt+line.
  write(2, "\n", 1);
  fd = open(dirpath, O_RDONLY);
  if(fd >= 0){
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      char name[DIRSIZ+1];
      memset(name, 0, sizeof(name));
      for(int i = 0; i < DIRSIZ; i++)
        name[i] = de.name[i];
      name[DIRSIZ] = 0;
      if(!starts_with(name, prefix))
        continue;
      printf("%s\n", name);
    }
    close(fd);
  }
  write(2, "$ ", 2);
  if(*lenp > 0)
    write(2, buf, *lenp);
}

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);
void runcmd(struct cmd*) __attribute__((noreturn));

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    exit(1);

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

int
getcmd(char *buf, int nbuf)
{
  int len = 0;
  int hist_offset = -1;
  char saved[HISTORY_CMDLEN];
  int saved_len = 0;

  write(2, "$ ", 2);
  memset(buf, 0, nbuf);
  memset(saved, 0, sizeof(saved));

  for(;;){
    char c;
    int cc = read(0, &c, 1);
    if(cc < 1)
      return -1;

    if(c == '\r')
      c = '\n';

    // Enter.
    if(c == '\n'){
      if(len < nbuf - 1)
        buf[len++] = '\n';
      buf[len] = 0;
      // console driver already echoes newline
      return 0;
    }

    // Backspace/Delete.
    if(c == 127 || c == '\b'){
      if(len > 0){
        len--;
        buf[len] = 0;
        // console driver already echoes backspace
      }
      continue;
    }

    // Arrow keys: ESC [ A (up) and ESC [ B (down)
    if(c == 0x1b){
      char c1, c2;
      if(read(0, &c1, 1) < 1)
        continue;
      if(c1 != '[')
        continue;
      if(read(0, &c2, 1) < 1)
        continue;

      if(c2 == 'A'){
        // Up arrow: older commands.
        int n = history_len();
        if(n == 0)
          continue;
        if(hist_offset == -1){
          saved_len = len;
          if(saved_len > HISTORY_CMDLEN - 1)
            saved_len = HISTORY_CMDLEN - 1;
          for(int i = 0; i < saved_len; i++)
            saved[i] = buf[i];
          saved[saved_len] = 0;
        }
        if(hist_offset < n - 1){
          hist_offset++;
          line_set(buf, nbuf, &len, history_get(hist_offset));
        }
        continue;
      }

      if(c2 == 'B'){
        // Down arrow: newer commands.
        if(hist_offset == -1)
          continue;
        if(hist_offset > 0){
          hist_offset--;
          line_set(buf, nbuf, &len, history_get(hist_offset));
        } else {
          // Back to the in-progress line.
          hist_offset = -1;
          line_set(buf, nbuf, &len, saved);
        }
        continue;
      }

      continue;
    }

    // Tab completion.
    if(c == '\t'){
      tab_complete(buf, nbuf, &len);
      continue;
    }

    // Normal character.
    if(len < nbuf - 2){
      buf[len++] = c;
      buf[len] = 0;
      // console driver already echoes characters
    }
  }
}

int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  history_load();

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    char *cmd = buf;
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    if (*cmd == '\n') // is a blank command
      continue;

    // Built-in: history
    if(cmd[0] == 'h' && cmd[1] == 'i' && cmd[2] == 's' && cmd[3] == 't' &&
       cmd[4] == 'o' && cmd[5] == 'r' && cmd[6] == 'y' &&
       (cmd[7] == '\n' || cmd[7] == 0)){
      history_print();
      history_add(cmd);
      continue;
    }

    // Add to history (stores without trailing newline).
    history_add(cmd);

    if(cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' '){
      // Chdir must be called by the parent, not the child.
      cmd[strlen(cmd)-1] = 0;  // chop \n
      if(chdir(cmd+3) < 0)
        fprintf(2, "cannot cd %s\n", cmd+3);
    } else {
      if(fork1() == 0)
        runcmd(parsecmd(cmd));
      wait(0);
    }
  }

  history_save();
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
