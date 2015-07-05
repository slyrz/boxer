#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define length(arr) \
  (sizeof (arr) / sizeof (arr[0]))

#define zero(var) \
  memset (&var, 0, sizeof (var))

#define default_value(var,val) \
  ((var) = (((var) == (typeof (var)) 0) ? (val) : (var)))

#define debug(...)      print_message (LOG_DEBUG, __VA_ARGS__)
#define info(...)       print_message (LOG_INFO, __VA_ARGS__)
#define warning(...)    print_message (LOG_WARNING, __VA_ARGS__)
#define error(...)      print_message (LOG_ERROR, __VA_ARGS__)

#define stop(...) \
  do { \
    warning (__VA_ARGS__); return; \
  } while (0)

#define fatal(...) \
  do { \
    error (__VA_ARGS__); exit (EXIT_FAILURE); \
  } while (0)

enum {
  LOG_DEBUG = 0,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
};

enum {
  OPTION_UNKOWN = 0,
  OPTION_BIND,
  OPTION_BIND_RO,
  OPTION_DOMAIN,
  OPTION_HELP,
  OPTION_HOME,
  OPTION_HOST,
  OPTION_IMAGE,
  OPTION_ROOT,
  OPTION_USER,
  OPTION_VERSION,
  OPTION_WORK,
  OPTION_CGROUP,
  OPTION_RLIMIT,
};

enum {
  USLEEP_MILLISECONDS = 1000,
  USLEEP_SECONDS      = 1000 * 1000,
};

struct mount {
  char *source;
  char *type;
  char *target;
  char *data;
  unsigned long flags;
};

struct device {
  char *name;
  char *path;
  unsigned int maj;
  unsigned int min;
  dev_t dev;
  mode_t mode;
};

static struct boxer {
  char *id;
  struct boxer_fd {
    int epoll;
    int signal;
  } fd;
  bool tty;
} boxer;

static struct container {
  struct container_user {
    uid_t uid;
    gid_t gid;
    char *name;
    char *home;
    char *shell;
  } user;
  struct container_path {
    char *console;
    char *home;
    char *image;
    char *root;
    char *work;
  } path;
  struct container_uts {
    const char *host;
    const char *domain;
  } uts;
  struct container_rlimit {
    char *name;
    long soft;
    long hard;
  } *rlimit;
  struct container_cgroup {
    char *subsystem;
    char *parameter;
    char *value;
    struct {
      char *subsystem;
      char *hierarchy;
      char *parameter;
      char *tasks;
    } path;
  } *cgroup;
  struct mount *bind;
  char **cmd;
} container;

static struct console {
  int master;
  int slave;
  int stdin;
  int stdout;
  struct console_buffer {
    size_t len;
    char data[LINE_MAX];
  } inp, out;
  struct console_attr {
    struct termios stdin;
    struct termios stdout;
    struct console_attr_saved {
      bool stdin;
      bool stdout;
    } saved;
  } attr;
} console;

static struct path {
  struct path_sync {
    const char *src;
    const char *dst;
  } sync;
} path;

static const struct mount mounts[] = {
  {"/bin", NULL, NULL, NULL, MS_BIND | MS_RDONLY | MS_NOSUID},
  {"/dev", "tmpfs", NULL, "mode=755", MS_NOSUID},
  {"/dev/pts", "devpts", NULL, "newinstance,ptmxmode=0666,mode=0620,gid=5", MS_NOEXEC | MS_NOSUID},
  {"/dev/shm", "tmpfs", NULL, "mode=1777,size=65536k", MS_NOEXEC | MS_NOSUID | MS_NODEV},
  {"/etc", NULL, NULL, NULL, MS_BIND | MS_RDONLY | MS_NOEXEC | MS_NOSUID},
  {"/lib", NULL, NULL, NULL, MS_BIND | MS_RDONLY | MS_NOSUID},
  {"/lib64", NULL, NULL, NULL, MS_BIND | MS_RDONLY | MS_NOSUID},
  {"/proc", "proc", NULL, NULL, MS_NOEXEC | MS_NOSUID | MS_NODEV},
  {"/run", "tmpfs", NULL, "mode=755", MS_NOSUID | MS_NODEV},
  {"/sys", "sysfs", NULL, NULL, MS_NOEXEC | MS_NOSUID | MS_NODEV | MS_RDONLY},
  {"/sys/fs/cgroup", "tmpfs", NULL, "mode=755", MS_NOEXEC | MS_NOSUID | MS_NODEV},
  {"/tmp", "tmpfs", NULL, "mode=1777", MS_NOSUID | MS_NODEV},
  {"/usr/bin", NULL, NULL, NULL, MS_BIND | MS_RDONLY | MS_NOSUID},
  {"/usr/lib", NULL, NULL, NULL, MS_BIND | MS_RDONLY | MS_NOSUID},
  {"/usr/share", NULL, NULL, NULL, MS_BIND | MS_RDONLY | MS_NOSUID},
};

static const struct device devices[] = {
  {"/dev/null", NULL, 0x1, 0x3, 0, 0},
  {"/dev/console", NULL, 0x1, 0x3, 0, 0666},
  {"/dev/zero", NULL, 0x1, 0x5, 0, 0},
  {"/dev/full", NULL, 0x1, 0x7, 0, 0},
  {"/dev/tty", NULL, 0x5, 0x0, 0, 0},
  {"/dev/random", NULL, 0x1, 0x8, 0, 0},
  {"/dev/urandom", NULL, 0x1, 0x9, 0, 0},
};

static void print_message (int, const char *, ...);
static void print_help (void);
static void print_version (void);

static void fd_block (int, bool);

static char *path_clean (const char *);
static void path_create (const char *);
static bool path_exists (const char *);
static void path_iterate (const char *, void (*)(const char *));
static char *path_join (const char *, ...);
static int path_sync (const char *, const char *);
static void path_write (const char *, const char *, ...);

static bool str_equals (const char *, const char *);
static char *str_random (const char *, size_t);
static void str_split_at (const char *, int, char **, char **);
static bool str_starts_with (const char *, const char *);
static long int str_to_long (const char *);

static void options_parse (int, char *const[]);
static void options_set (int, const char *, char *);
static void options_set_bind_mount (const char *, bool);
static void options_set_cgroup (const char *, char *);
static void options_set_rlimit (const char *, char *);

static void device_setup (const struct device *);
static void mount_setup (const struct mount *);

static void console_buffer_pipe (struct console_buffer *, int, int);
static void console_forward_size (int, int);
static void console_init (void);
static void console_make_raw (int, struct termios *);
static void console_restore (void);
static void console_setup (void);
static void console_setup_master (void);
static void console_setup_slave (void);

static bool container_image_contains (const char *);
static void container_init (void);
static void container_kill (void);
static void container_run (void);
static void container_setup (void);
static void container_setup_cgroup (void);
static void container_setup_rlimit (void);

static void boxer_fd_poll (int);
static void boxer_fd_unpoll (int);
static void boxer_init (void);
static void boxer_run (void);
static void boxer_setup (void);
static void boxer_signal (void);

static void
print_message (int level, const char *format, ...)
{
#define item(level,name,color) [level] = { name, "\x1b[" #color ";1m" name "\x1b[0m" }
  const char *names[][2] = {
    item (LOG_DEBUG, "dbg", 34),
    item (LOG_INFO, "inf", 32),
    item (LOG_WARNING, "wrn", 33),
    item (LOG_ERROR, "err", 31),
  };
#undef item

  va_list ap;
  va_start (ap, format);

  fprintf (stderr, " %.8s | %s ~ ", boxer.id, names[level][boxer.tty]);
  vfprintf (stderr, format, ap);
  if (errno) {
    if (boxer.tty)
      fprintf (stderr, ": \x1b[33m%s\x1b[0m", strerror (errno));
    else
      fprintf (stderr, ": %s", strerror (errno));
    errno = 0;
  }
  fputc ('\n', stderr);
  va_end (ap);
}

static void
print_help (void)
{
  printf ("Call: %s [OPTION]... [COMMAND]\n"
          "Execute a command or run a shell inside a container.\n"
          "\n"
          "Options:\n"
          "  -h, --help               Print this help and exit\n"
          "  -v, --version            Print version information and exit\n"
          "  -b, --bind=SRC[:DST]     Bind SRC to a path DST in container\n"
          "  -B, --bind-ro=SRC[:DST]  Bind SRC read-only to a path DST in container\n"
          "  -d, --domain=NAME        Domainname in container\n"
          "  -H, --home=DIR           Home directory in container\n"
          "      --host=NAME          Hostname in container\n"
          "  -i, --image=DIR          Image of the root filesystem\n"
          "  -r, --root=DIR           Root directory\n"
          "  -u, --user=NAME          User in container\n"
          "  -w, --work=DIR           Working directory in container\n"
          "\n"
          "Cgroup Options:\n"
          "      --cgroup.SUBSYSTEM.PARAMETER=VALUE\n"
          "\n"
          "Rlimit Options:\n"
          "      --rlimit.RESOURCE=HARD\n"
          "      --rlimit.RESOURCE=SOFT/HARD\n"
          "",
          program_invocation_short_name);
}

static void
print_version (void)
{
  printf ("%s version 0.1\n", program_invocation_short_name);
}

/**
 * fd_block sets a file descriptor fd to blocking/nonblocking mode.
 */
static void
fd_block (int fd, bool block)
{
  int flags;

  flags = fcntl (fd, F_GETFL, 0);
  if (flags < 0)
    fatal ("fcntl");

  if (block)
    flags &= ~O_NONBLOCK;
  else
    flags |= O_NONBLOCK;

  if (fcntl (fd, F_SETFL, flags) != 0)
    fatal ("fcntl");
}

/**
* path_clean removes consecutive and trailing directory separators.
* Both do no harm, they just look ugly in the logs.
* It's a simple alternative to realpath and canonicalize_file_name, which are
* limited because they require their arguments to be existing paths.
*/
static char *
path_clean (const char *path)
{
  char *result;
  char *end;
  char *pos;

  result = strdup (path);
  if (result == NULL)
    fatal ("strdup");
  pos = end = result;
  while (*end) {
    *pos = *end;
    if (*end == '/')
      while (*end == '/')
        end++;
    else
      end++;
    pos++;
  }
  /**
   * Remove trailing slash if path is not "/".
   */
  if (((pos - result) > 1) && (pos[-1] == '/'))
    pos--;
  *pos = '\0';

  return result;
}

static inline void
path_create_dir (const char *path)
{
  const mode_t mode = 0755;

  if (mkdir (path, mode) != 0)
    if (errno != EEXIST)
      fatal ("mkdir %s mode=%#o", path, mode);
  errno = 0;
}

static void
path_create (const char *path)
{
  path_iterate (path, path_create_dir);
}

static bool
path_exists (const char *path)
{
  struct stat buf;
  bool found;

  found = (stat (path, &buf) == 0);
  errno = 0;
  return found;
}

static void
path_iterate (const char *str, void (*callback) (const char *))
{
  char *var;
  char *pos;

  var = strdup (str);
  if (var == NULL)
    fatal ("strdup");

  /**
   * Replace each path seperator with a null byte.
   */
  pos = var;
  while (*pos) {
    if (*pos == '/')
      *pos = '\0';
    pos++;
  }
  /**
   * Restore each path seperator one by one and call
   * callback on the results.
   */
  if (*var)
    callback (var);
  pos = var;
  while (*str) {
    if (*pos = *str, *pos == '/')
      callback (var);
    pos++;
    str++;
  }
  free (var);
}

static char *
path_join (const char *format, ...)
{
  va_list ap;
  char *str;

  va_start (ap, format);
  if (vasprintf (&str, format, ap) < 0)
    fatal ("vasprintf");
  va_end (ap);
  return path_clean (str);
}

static inline void
path_sync_reg (const char *dst, const char *src, const struct stat *sb)
{
  char buffer[2048];
  int ofd;
  int ifd;

  ifd = open (src, O_CLOEXEC | O_RDONLY);
  if (ifd < 0)
    fatal ("open %s", src);

  ofd = open (dst, O_CLOEXEC | O_CREAT | O_WRONLY | O_TRUNC);
  if (ofd < 0)
    fatal ("open %s", dst);

  ssize_t ret;
  for (;;) {
    ret = read (ifd, buffer, sizeof (buffer));
    if (ret <= 0)
      break;
    if (write (ofd, buffer, ret) != ret)
      fatal ("write");
  }
  if (close (ofd) != 0)
    fatal ("close %s", dst);
  close (ifd);
}

static inline void
path_sync_dir (const char *dst, const char *src, const struct stat *sb)
{
  if (mkdir (dst, sb->st_mode) != 0)
    fatal ("mkdir %s, mode=%#o", dst, sb->st_mode);
}

static inline void
path_sync_sym (const char *dst, const char *src, const struct stat *sb)
{
  char *target = NULL;

  target = calloc (sb->st_size + 1, sizeof (char));
  if (target == NULL)
    fatal ("calloc");
  if (readlink (src, target, sb->st_size + 1) != sb->st_size)
    fatal ("readlink %s", src);
  if (symlink (target, dst) != 0)
    fatal ("symlink %s %s", target, dst);
  free (target);
}

static inline int
path_sync_callback (const char *src, const struct stat *sb, int type, struct FTW *buf)
{
  const char *rel;
  char *dst;

  /**
   * Remove the starting directory from the source path, then join what's left
   * with the destination directory to obtain the destination path.
   */
  rel = src + strlen (path.sync.src);
  if (*rel == '\0')
    return 0;

  dst = path_join ("%s/%s", path.sync.dst, rel);
  switch (type) {
    case FTW_F:
      path_sync_reg (dst, src, sb);
      break;
    case FTW_D:
      path_sync_dir (dst, src, sb);
      break;
    case FTW_SL:
      path_sync_sym (dst, src, sb);
      break;
  }

  /**
   * Preserve mode and ownership. Use lchown for links, because
   * chown dereferences symbolic links.
   */
  if (type == FTW_SL) {
    lchown (dst, sb->st_uid, sb->st_gid);
  }
  else {
    chown (dst, sb->st_uid, sb->st_gid);
    chmod (dst, sb->st_mode);
  }
  free (dst);
  return 0;
}

static int
path_sync (const char *source, const char *target)
{
  path.sync.src = source;
  path.sync.dst = target;
  return nftw (path.sync.src, path_sync_callback, 32, FTW_PHYS);
}

static void
path_write (const char *path, const char *format, ...)
{
  va_list ap;
  int fd;

  fd = open (path, O_CLOEXEC | O_WRONLY);
  if (fd < 0)
    goto error;
  va_start (ap, format);
  if (vdprintf (fd, format, ap) <= 0)
    goto error;
  va_end (ap);
  if (close (fd) != 0)
    goto error;
  return;
error:
  fatal ("failed to write file %s", path);
}

static bool
str_equals (const char *str, const char *other)
{
  if (str == NULL || other == NULL)
    return 0;
  return strcmp (str, other) == 0;
}

static char *
str_random (const char *set, size_t n)
{
  char *result;
  int fd;

  size_t i;
  size_t l;

  result = calloc (n + 1, sizeof (char));
  if (result == NULL)
    fatal ("calloc");
  fd = open ("/dev/urandom", O_CLOEXEC | O_RDONLY);
  if (fd < 0)
    fatal ("open /dev/urandom");
  if (read (fd, result, n) != (ssize_t) n)
    fatal ("read");
  close (fd);
  l = strlen (set);
  for (i = 0; i < n; i++)
    result[i] = set[result[i] % l];
  return result;
}

static void
str_split_at (const char *str, int c, char **lo, char **hi)
{
  char *val;
  char *pos;

  val = strdup (str);
  if (val == NULL)
    fatal ("strdup");
  pos = strchr (val, c);
  if (pos) {
    *pos = '\0';
    *lo = val;
    *hi = pos + 1;
  }
  else {
    *lo = val;
    *hi = NULL;
  }
}

static bool
str_starts_with (const char *str, const char *prefix)
{
  if (str == NULL || prefix == NULL)
    return 0;
  return strncasecmp (str, prefix, strlen (prefix)) == 0;
}

static long int
str_to_long (const char *str)
{
  long int value;
  char *end;

  errno = 0;
  value = strtol (str, &end, 10);
  if (errno != 0)
    fatal ("strol %s", str);
  if (end != NULL) {
    switch (*end) {
      case 'g':
      case 'G':
        return value * 1024 * 1024 * 1024;
      case 'm':
      case 'M':
        return value * 1024 * 1024;
      case 'k':
      case 'K':
        return value * 1024;
    }
  }
  return value;
}

static void
options_parse (int argc, char *const argv[])
{
  static const struct {
    int id;
    char *longname;
    char *shortname;
    char *prefix;
  } options[] = {
    {OPTION_BIND,    "bind",    "b" ,  NULL},
    {OPTION_BIND_RO, "bind-ro", "B" ,  NULL},
    {OPTION_DOMAIN,  "domain",  NULL,  NULL},
    {OPTION_HELP,    "help",    "h" ,  NULL},
    {OPTION_HOME,    "home",    "H" ,  NULL},
    {OPTION_HOST,    "host",    NULL,  NULL},
    {OPTION_IMAGE,   "image",   "i" ,  NULL},
    {OPTION_ROOT,    "root",    "r" ,  NULL},
    {OPTION_USER,    "user",    "u" ,  NULL},
    {OPTION_VERSION, "version", "v" ,  NULL},
    {OPTION_WORK,    "work",    "w" ,  NULL},
    {OPTION_RLIMIT,  NULL,      NULL, "rlimit."},
    {OPTION_CGROUP,  NULL,      NULL, "cgroup."},
  };

  size_t i;
  size_t j;

  container.cgroup = calloc (argc, sizeof (struct container_cgroup));
  if (container.cgroup == NULL)
    fatal ("calloc");

  container.rlimit = calloc (argc, sizeof (struct container_rlimit));
  if (container.rlimit == NULL)
    fatal ("calloc");

  container.bind = calloc (argc, sizeof (struct mount));
  if (container.bind == NULL)
    fatal ("calloc");

  for (i = 1; argv[i] != NULL; i++) {
    char *name;
    char *argument;

    name = argv[i];
    if (name[0] != '-')
      break;

    /**
     * Allow --name argument and --name=argument.
     *
     * Assume that all options take arguments. Only two options take no
     * arguments: --help and --version. These two options exit the program
     * on their options_set() call, so it doesn't matter if we assign them
     * the next item in the argv array as argument.
     */
    str_split_at (name, '=', &name, &argument);
    if (argument == NULL)
      argument = argv[++i];

    /**
     * Skip the "-" or "--" prefixes of the option name.
     */
    name++;
    if (*name == '-') {
      name++;
      /**
       * Just like getopt and getopt_long, allow a single "--" to force
       * the end of option parsing.
       */
      if (*name == '\0')
        break;
    }

    for (j = 0; j < length (options); j++) {
      if (str_equals (name, options[j].shortname) || str_equals (name, options[j].longname))
        break;
      if (str_starts_with (name, options[j].prefix)) {
        name += strlen (options[j].prefix);
        break;
      }
    }

    if (j >= length (options))
      options_set (OPTION_UNKOWN, name, argument);
    else
      options_set (options[j].id, name, argument);
  }

  /**
   * What's left should be the command to run inside the container.
   */
  if (argv[i] != NULL)
    container.cmd = (char **) argv + i;
}

static void
options_set (int option, const char *name, char *value)
{
  switch (option) {
    case OPTION_HELP:
      print_help ();
      exit (0);
      break;
    case OPTION_VERSION:
      print_version ();
      exit (0);
      break;
    case OPTION_USER:
      container.user.name = value;
      break;
    case OPTION_HOST:
      container.uts.host = value;
      break;
    case OPTION_DOMAIN:
      container.uts.domain = value;
      break;
    case OPTION_IMAGE:
      container.path.image = value;
      break;
    case OPTION_ROOT:
      container.path.root = value;
      break;
    case OPTION_WORK:
      container.path.work = value;
      break;
    case OPTION_HOME:
      container.path.home = value;
      break;
    case OPTION_BIND:
    case OPTION_BIND_RO:
      options_set_bind_mount (value, option == OPTION_BIND_RO);
      break;
    case OPTION_RLIMIT:
      debug ("rlimit name='%s' value='%s'", name, value);
      options_set_rlimit (name, value);
      break;
    case OPTION_CGROUP:
      debug ("cgroup name='%s' value='%s'", name, value);
      options_set_cgroup (name, value);
      break;
    case OPTION_UNKOWN:
      warning ("Unknown option %s", name);
      break;
  }
}

static void
options_set_bind_mount (const char *value, bool readonly)
{
  size_t i;

  for (i = 0; container.bind[i].source != NULL; i++)
    ;

  str_split_at (value, ':', &container.bind[i].source, &container.bind[i].target);
  container.bind[i].flags = MS_BIND;
  if (readonly)
    container.bind[i].flags |= MS_RDONLY;
}

static void
options_set_cgroup (const char *name, char *value)
{
  char *subsystem;
  char *parameter;
  size_t i;

  if (sscanf (name, "%m[^.].%ms", &subsystem, &parameter) != 2)
    stop ("sscanf %%m[^.].%%ms %s", name);

  for (i = 0; container.cgroup[i].subsystem != NULL; i++) {
    if (!str_equals (subsystem, container.cgroup[i].subsystem))
      continue;
    if (!str_equals (parameter, container.cgroup[i].parameter))
      continue;
    break;
  }

  container.cgroup[i].subsystem = subsystem;
  container.cgroup[i].parameter = parameter;
  container.cgroup[i].value = value;
}

static void
options_set_rlimit (const char *name, char *value)
{
  char *hard;
  char *soft;
  size_t i;

  for (i = 0; container.rlimit[i].name != NULL; i++)
    if (strcasecmp (name, container.rlimit[i].name) == 0)
      break;

  str_split_at (value, '/', &soft, &hard);
  if (hard == NULL)
    hard = soft;
  container.rlimit[i] = (struct container_rlimit){
    .name = (char *) name,
    .soft = str_to_long (soft),
    .hard = str_to_long (hard)
  };
}

static void
device_setup (const struct device *dev)
{
  struct device d = *dev;
  struct stat sb;

  if (stat (d.name, &sb) != 0)
    fatal ("stat %s", d.name);

  default_value (d.mode, sb.st_mode);
  default_value (d.path, path_join ("%s/%s", container.path.root, d.name));
  default_value (d.dev, makedev (d.maj, d.min));

  info ("Creating %s", dev->name);
  if (mknod (d.path, d.mode, d.dev) != 0)
    fatal ("mknod %s in %s", d.name, d.path);
  if (chown (d.path, sb.st_uid, sb.st_gid) != 0)
    fatal ("chown %s uid=%sb.st_gid=%d", d.path, sb.st_uid, sb.st_gid);
}

static void
mount_setup (const struct mount *mnt)
{
  struct mount m = *mnt;

  default_value (m.target, path_join ("%s/%s", container.path.root, m.source));
  default_value (m.data, "");
  default_value (m.type, "");

  /**
   * If the user provides any of the default mounts with the container image, e.g.
   * folders like /bin or /etc, do not bind mount the system directories
   * and leave the contents up to the user.
   */
  if (container_image_contains (m.target))
    stop ("Skipping %s because it's part of the container image", m.source);

  info ("Mounting %s", m.source);
  path_create (m.target);
  if (mount (m.source, m.target, m.type, m.flags, m.data) != 0) {
    if (errno == ENOENT)
      stop ("mount %s %s", m.source, m.target);
    else
      fatal ("mount %s %s", m.source, m.target);
  }
  /**
   * If mount is a bind mount with additional options, the first mount created a bind
   * mount with the options of the original mount point. To use the mount options of
   * the user, a remount needs to be done.
   */
  if ((m.flags & MS_BIND) && (m.flags != MS_BIND))
    if (mount (NULL, m.target, m.type, m.flags | MS_REMOUNT, m.data) != 0)
      fatal ("mount %s %s", m.source, m.target);
}

static bool
container_image_contains (const char *path)
{
  char *image_path;
  bool result;

  if (container.path.image == NULL)
    return false;
  image_path = path_join ("%s/%s", container.path.image, path);
  result = path_exists (image_path);
  free (image_path);
  return result;
}

static void
container_init (void)
{
  struct passwd *pwd;
  size_t i;

  /**
   * Get information about the container user.
   */
  if (container.user.name)
    pwd = getpwnam (container.user.name);
  else
    pwd = getpwuid (getuid ());

  if (pwd == NULL)
    fatal ("getpw failed");

  container.user = (struct container_user){
    .name = strdup (pwd->pw_name),
    .home = strdup (pwd->pw_dir),
    .shell = strdup (pwd->pw_shell),
    .uid = pwd->pw_uid,
    .gid = pwd->pw_gid,
  };

  /**
   * Set the default paths.
   */
  default_value (container.path.root, path_join ("/var/boxer/%s/", boxer.id));
  default_value (container.path.home, container.user.home);
  default_value (container.path.work, container.path.home);

  /**
   * Now that container.path.root is known, place user-defined mount targets
   * in the container root directory.
   */
  for (i = 0; container.bind[i].source != NULL; i++)
    if (container.bind[i].target)
      container.bind[i].target = path_join ("%s/%s", container.path.root, container.bind[i].target);

  /**
   * If the user did not provided a command, run the user' shell instead.
   */
  if (container.cmd == NULL) {
    container.cmd = calloc (2, sizeof (char *));
    if (container.cmd == NULL)
      fatal ("calloc");
    container.cmd[0] = container.user.shell;
  }
}

/**
 * container_kill reads the cgroups tasks file and kills all processes besides
 * the calling process.
 */
static void
container_kill (void)
{
  const int signum = SIGKILL;
  unsigned long num;
  char *path;
  FILE *f;

  pid_t self = getpid ();
  pid_t child;

  path = path_join ("/sys/fs/cgroup/boxer/%s/tasks", boxer.id);
  for (;;) {
    int killed;

    f = fopen (path, "re");
    if (f == NULL)
      fatal ("fopen %s", path);
    killed = 0;
    for (;;) {
      if (fscanf (f, "%lu", &num) != 1)
        break;
      child = (pid_t) num;
      if (child == self)
        continue;
      killed++;
      if (kill (child, signum) != 0)
        warning ("kill");
    }
    if (!feof (f))
      fatal ("failed to read all pids");
    fclose (f);
    /**
     * It's safe to exit once the tasks file contained only this process' pid.
     */
    if (killed == 0)
      break;
    /**
     * If one or more processes were killed, give the system some time and see
     * if new processes are still being created.
     */
    usleep (100 * USLEEP_MILLISECONDS);
  }
  free (path);

  while (waitpid (-1, 0, WNOHANG) > 0);
}

static void
container_run (void)
{
  /**
   * Drop the root rights, then run the command.
   */
  if (setgid (container.user.gid) != 0)
    fatal ("setgid");
  if (setuid (container.user.uid) != 0)
    fatal ("setuid");
  if (setuid (0) == 0)
    fatal ("permissions restorable");
  if (execv (container.cmd[0], container.cmd) != 0)
    fatal ("execv");
}

static void
container_setup (void)
{
  char *path;
  size_t i;

  path_create (container.path.root);

  /**
   * Do not propagate mounts to or from the real root.
   */
  mount_setup (&(struct mount){
    .target = "/",
    .flags  = MS_PRIVATE | MS_REC,
  });

  mount_setup (&(struct mount){
    .source = "tmpfs",
    .target = container.path.root,
    .type   = "tmpfs",
    .data   = "size=512",
    .flags  = MS_NOSUID,
  });

  if (container.path.image) {
    info ("Creating a copy of %s as root filesystem in %s", container.path.image, container.path.root);
    path_sync (container.path.image, container.path.root);
  }

  if (container.uts.host)
    sethostname (container.uts.host, strlen (container.uts.host));
  if (container.uts.domain)
    setdomainname (container.uts.domain, strlen (container.uts.domain));

  for (i = 0; i < length (mounts); i++)
    mount_setup (mounts + i);

  mode_t u = umask (0000);
  for (i = 0; i < length (devices); i++)
    device_setup (devices + i);
  umask (u);

  for (i = 0; container.bind[i].source != NULL; i++)
    mount_setup (container.bind + i);

  path = path_join ("%s/dev/ptmx", container.path.root);
  if (symlink ("pts/ptmx", path) != 0)
    fatal ("symlink pts/ptmx %s", path);
  free (path);

  path = path_join ("%s/dev/pts/ptmx", container.path.root);
  if (chmod (path, 0666) != 0)
    fatal ("chmod %s", path);
  free (path);

  if (container.path.console)
    mount_setup (&(struct mount){
      .source = container.path.console,
      .target = path_join ("%s/dev/console", container.path.root),
      .flags  = MS_BIND,
    });

  /**
   * Change the root directory.
   */
  info ("Entering container");
  if (chroot (container.path.root) != 0)
    fatal ("chroot");
  if (chdir ("/") != 0)
    fatal ("chdir /");

  /**
   *  - /dev/fd -> /proc/self/fd
   *  - /dev/stdin -> /proc/self/fd/0
   *  - /dev/stdout -> /proc/self/fd/1
   *  - /dev/stderr -> /proc/self/fd/2
   */
  if (symlink ("/proc/self/fd", "/dev/fd") != 0)
    fatal ("symlink /dev/fd");
  if (symlink ("/proc/self/fd/0", "/dev/stdin") != 0)
    fatal ("symlink /dev/fd");
  if (symlink ("/proc/self/fd/1", "/dev/stdout") != 0)
    fatal ("symlink /dev/fd");
  if (symlink ("/proc/self/fd/2", "/dev/stderr") != 0)
    fatal ("symlink /dev/fd");

  if (!path_exists (container.path.home)) {
    path_create (container.path.home);
    if (chown (container.path.home, container.user.uid, container.user.gid) != 0)
      fatal ("chown %s", container.path.home);
  }

  if (!path_exists (container.path.work)) {
    path_create (container.path.work);
    if (chown (container.path.work, container.user.uid, container.user.gid) != 0)
      fatal ("chown %s", container.path.work);
  }

  info ("Changing working directory to %s", container.path.work);
  if (chdir (container.path.work) != 0)
    fatal ("chdir %s", container.path.work);

  /**
   * This should happen after entering the container. Otherwise the user
   * inside the container sees an empty /sys/fs/cgroups directory.
   */
  container_setup_cgroup ();
  container_setup_rlimit ();

  umask (0022);
}

static void
container_setup_cgroup (void)
{
  struct container_cgroup *cgroup;
  size_t i;
  pid_t pid;

  pid = getpid ();
  for (i = 0; container.cgroup[i].subsystem != NULL; i++) {
    cgroup = container.cgroup + i;
    default_value (cgroup->path.subsystem, path_join ("/sys/fs/cgroup/%s", cgroup->subsystem));
    default_value (cgroup->path.hierarchy, path_join ("%s/boxer/%s", cgroup->path.subsystem, boxer.id));
    default_value (cgroup->path.parameter, path_join ("%s/%s.%s", cgroup->path.hierarchy, cgroup->subsystem, cgroup->parameter));
    default_value (cgroup->path.tasks, path_join ("%s/tasks", cgroup->path.hierarchy));

    if (!path_exists (cgroup->path.subsystem))
      mount_setup (&(struct mount){
        .source = "cgroup",
        .target = cgroup->path.subsystem,
        .type   = "cgroup",
        .data   = cgroup->subsystem,
      });

    path_create (cgroup->path.hierarchy);
    path_write (cgroup->path.parameter, "%s\n", cgroup->value);
    path_write (cgroup->path.tasks, "%d\n", pid);
  }
}

static void
container_setup_rlimit (void)
{
#define item(name) [RLIMIT_ ## name] = #name
  static const char *names[] = {
    item (CPU),
    item (FSIZE),
    item (DATA),
    item (STACK),
    item (CORE),
    item (RSS),
    item (NOFILE),
    item (AS),
    item (NPROC),
    item (MEMLOCK),
    item (LOCKS),
    item (SIGPENDING),
    item (MSGQUEUE),
    item (NICE),
    item (RTPRIO),
    item (RTTIME),
  };
#undef item

  size_t i;
  size_t j;

  for (i = 0; container.rlimit[i].name != NULL; i++) {
    for (j = 0; j < length (names); j++)
      if (names[j] && strcasecmp (names[j], container.rlimit[i].name) == 0)
        break;
    if (j == length (names))
      fatal ("Unknown rlimit %s", container.rlimit[i].name);

    struct rlimit rlimit = {
      .rlim_cur = container.rlimit[i].soft,
      .rlim_max = container.rlimit[i].hard,
    };
    if (setrlimit (j, &rlimit) != 0)
      fatal ("setrlimit %s", names[j]);
  }
}

static void
console_buffer_pipe (struct console_buffer *buffer, int source, int target)
{
  ssize_t ret;

  ret = read (source, buffer->data + buffer->len, sizeof (buffer->data) - buffer->len);
  if (ret <= 0) {
    if (errno != EAGAIN && errno != EINTR)
      boxer_fd_unpoll (source);
  }
  else
    buffer->len += (size_t) ret;

  if (buffer->len > 0) {
    ret = write (target, buffer->data, buffer->len);
    if (ret > 0) {
      memmove (buffer->data, buffer->data + ret, buffer->len - ret);
      buffer->len -= ret;
    }
  }
  errno = 0;
}

/**
 * console_forward_size sets the window size of the terminal under the target file descriptor
 * to the window size of the terminal under the source file descriptor.
 */
static void
console_forward_size (int source, int target)
{
  struct winsize ws;

  if (ioctl (source, TIOCGWINSZ, &ws) >= 0)
    ioctl (target, TIOCSWINSZ, &ws);
}

static void
console_init (void)
{
  console.stdin = STDIN_FILENO;
  console.stdout = STDOUT_FILENO;
}

static void
console_make_raw (int fd, struct termios *attr)
{
  struct termios raw = *attr;

  if (tcgetattr (fd, attr) != 0)
    fatal ("tcgetattr");
  cfmakeraw (&raw);
  switch (fd) {
    case STDIN_FILENO:
      raw.c_oflag = attr->c_oflag;
      break;
    case STDOUT_FILENO:
      raw.c_iflag = attr->c_iflag;
      raw.c_lflag = attr->c_lflag;
      break;
  }
  tcsetattr (fd, TCSANOW, &raw);
}

static void
console_restore (void)
{
  console_buffer_pipe (&console.out, console.master, console.stdout);
  if (console.attr.saved.stdout)
    tcsetattr (console.stdout, TCSANOW, &console.attr.stdout);
  if (console.attr.saved.stdin)
    tcsetattr (console.stdin, TCSANOW, &console.attr.stdin);
  fd_block (console.stdout, true);
  fd_block (console.stdin, true);
}

static void
console_setup (void)
{
  console.master = posix_openpt (O_RDWR | O_NOCTTY | O_CLOEXEC | O_NDELAY);
  if (console.master < 0)
    fatal ("posix_openpt");
  container.path.console = ptsname (console.master);
  if (container.path.console == NULL)
    fatal ("ptsname");
  container.path.console = strdup (container.path.console);
  if (container.path.console == NULL)
    fatal ("strdup");
  if (chmod (container.path.console, 0600) != 0)
    fatal ("chmod %s", container.path.console);
  if (chown (container.path.console, 0, 0) != 0)
    fatal ("chown %s", container.path.console);
  if (unlockpt (console.master) != 0)
    fatal ("unlockpt");
}

static void
console_setup_master (void)
{
  fd_block (console.stdin, false);
  fd_block (console.stdout, false);
  fd_block (console.master, false);

  console_forward_size (console.stdout, console.master);
  console_make_raw (console.stdin, &console.attr.stdin);
  console_make_raw (console.stdout, &console.attr.stdout);
  console.attr.saved.stdout = true;
  console.attr.saved.stdin = true;
}

static void
console_setup_slave (void)
{
  close (console.master);
  console.slave = open (container.path.console, O_RDWR);
  if (console.slave < 0)
    fatal ("open %s", container.path.console);

  /**
   * Make slave the controlling terminal of this process.
   */
  if (ioctl (console.slave, TIOCSCTTY, 0) == -1)
    fatal ("ioctl");
  if (dup2 (console.slave, STDIN_FILENO) != STDIN_FILENO)
    fatal ("dup2 console.slave STDIN");
  if (dup2 (console.slave, STDOUT_FILENO) != STDOUT_FILENO)
    fatal ("dup2 console.slave STDOUT");
  if (dup2 (console.slave, STDERR_FILENO) != STDERR_FILENO)
    fatal ("dup2 console.slave STDERR");

  fchown (STDIN_FILENO, container.user.uid, container.user.gid);
  fchown (STDOUT_FILENO, container.user.uid, container.user.gid);
  fchown (STDERR_FILENO, container.user.uid, container.user.gid);
}

static void
boxer_fd_poll (int fd)
{
  struct epoll_event ev;

  zero (ev);
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = fd;
  if (epoll_ctl (boxer.fd.epoll, EPOLL_CTL_ADD, fd, &ev) != 0)
    fatal ("epoll_ctl EPOLL_CTL_ADD");
}

static void
boxer_fd_unpoll (int fd)
{
  if (epoll_ctl (boxer.fd.epoll, EPOLL_CTL_DEL, fd, NULL) != 0)
    fatal ("epoll_ctl EPOLL_CTL_DEL");
}

static void
boxer_init (void)
{
  /**
   * Each boxer process needs a unique ID to create separate cgroup hierarchies.
   * The boxer PID can't be used, because PIDs might clash if boxer processes are
   * run in different PID namespaces. That's why the ID is just a large random
   * string.
   */
  boxer.id = str_random ("abcdefghijklmnopqrstuvwxyz0123456789", 20);
  boxer.tty = isatty (STDOUT_FILENO);
}

static void
boxer_run (void)
{
  sigset_t mask;

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);
  sigaddset (&mask, SIGINT);
  sigaddset (&mask, SIGTERM);
  sigaddset (&mask, SIGWINCH);
  if (sigprocmask (SIG_BLOCK, &mask, NULL) == -1)
    fatal ("sigprocmask");

  boxer.fd.signal = signalfd (-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (boxer.fd.signal == -1)
    fatal ("signalfd");

  boxer.fd.epoll = epoll_create1 (0);
  if (boxer.fd.epoll < 0)
    fatal ("epoll_create1");

  boxer_fd_poll (boxer.fd.signal);
  boxer_fd_poll (console.stdin);
  boxer_fd_poll (console.master);

  for (;;) {
    struct epoll_event events[16];
    int i;
    int n;

    n = epoll_wait (boxer.fd.epoll, events, length (events), -1);
    if (n == -1)
      fatal ("epoll_wait");
    for (i = 0; i < n; ++i) {
      if (events[i].data.fd == boxer.fd.signal)
        boxer_signal ();

      if (events[i].data.fd == console.stdin)
        console_buffer_pipe (&console.inp, console.stdin, console.master);
      if (events[i].data.fd == console.master)
        console_buffer_pipe (&console.out, console.master, console.stdout);
    }
  }
}

static void
boxer_setup (void)
{
  char *path;

  /**
   * Setup a cgroup for all boxer processes. This happens outside the container.
   * The cgroup subsystem will be used to keep track of the container processes
   * via the cgroup tasks files.
   */
  if (!path_exists ("/sys/fs/cgroup/boxer"))
    mount_setup (&(struct mount){
      .source = "cgroup",
      .target = "/sys/fs/cgroup/boxer",
      .type   = "cgroup",
      .data   = "none,name=boxer,xattr",
      .flags  = MS_NOSUID | MS_NOEXEC | MS_NODEV,
      });

  path = path_join ("/sys/fs/cgroup/boxer/%s", boxer.id);
  path_create (path);
  free (path);

  path = path_join ("/sys/fs/cgroup/boxer/%s/tasks", boxer.id);
  path_write (path, "%d\n", getpid ());
  free (path);
}

static void
boxer_signal (void)
{
  struct signalfd_siginfo sig;
  ssize_t ret;
  int status = EXIT_FAILURE;

  ret = read (boxer.fd.signal, &sig, sizeof (struct signalfd_siginfo));
  if (ret != sizeof (struct signalfd_siginfo))
    fatal ("read signalfd");

  switch (sig.ssi_signo) {
    case SIGWINCH:
      console_forward_size (console.stdout, console.master);
      break;
    case SIGCHLD:
      status = sig.ssi_status; // fallthrough
    case SIGINT:
    case SIGTERM:
      container_kill ();
      console_restore ();
      exit (status);
  }
}

int
main (int argc, char *const argv[])
{
  pid_t pid;

  zero (boxer);
  zero (console);
  zero (container);

  options_parse (argc, argv);

  boxer_init ();
  console_init ();
  container_init ();

  info ("Boxer ID: %s", boxer.id);
  info ("User: %s (uid=%d, gid=%d)", container.user.name, container.user.uid, container.user.gid);
  info ("Root: %s", container.path.root);
  info ("Home: %s", container.path.home);

  boxer_setup ();
  console_setup ();

  /**
   * These namespaces will be active in the forked child process.
   */
  if (unshare (CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS) != 0)
    fatal ("unshare");

  pid = fork ();
  if (pid == -1)
    fatal ("fork");
  if (pid == 0) {
    if (setsid () < 0)
      fatal ("setsid");
    console_setup_slave ();
    container_setup ();
    container_run ();
  }
  else {
    console_setup_master ();
    boxer_run ();
  }
  return 0;
}
