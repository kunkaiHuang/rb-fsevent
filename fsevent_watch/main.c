#include "common.h"
#include "cli.h"

// Structure for storing metadata parsed from the commandline
static struct {
  FSEventStreamEventId            sinceWhen;
  CFTimeInterval                  latency;
  FSEventStreamCreateFlags        flags;
  CFMutableArrayRef               paths;
  enum FSEventWatchOutputFormat   format;
} config = {
  (UInt64) kFSEventStreamEventIdSinceNow,
  (double) 0.3,
  (UInt32) kFSEventStreamCreateFlagNone,
  NULL,
  kFSEventWatchOutputFormatClassic
};

// Prototypes
static void         append_path(const char *path);
static inline void  parse_cli_settings(int argc, const char *argv[]);
static void         callback(FSEventStreamRef streamRef,
                             void *clientCallBackInfo,
                             size_t numEvents,
                             void *eventPaths,
                             const FSEventStreamEventFlags eventFlags[],
                             const FSEventStreamEventId eventIds[]);

// Resolve a path and append it to the CLI settings structure
// The FSEvents API will, internally, resolve paths using a similar scheme.
// Performing this ahead of time makes things less confusing, IMHO.
static void append_path(const char *path)
{
#ifdef DEBUG
  fprintf(stderr, "\n");
  fprintf(stderr, "append_path called for: %s\n", path);
#endif
  
  char fullPath[PATH_MAX];
  
  if (realpath(path, fullPath) == NULL) {
#ifdef DEBUG
    fprintf(stderr, "  realpath not directly resolvable from path\n");
#endif
    
    if (path[0] != '/') {
#ifdef DEBUG
      fprintf(stderr, "  passed path is not absolute\n");
#endif
      size_t len;
      getcwd(fullPath, sizeof(fullPath));
#ifdef DEBUG
      fprintf(stderr, "  result of getcwd: %s\n", fullPath);
#endif
      len = strlen(fullPath);
      fullPath[len] = '/';
      strlcpy(&fullPath[len + 1], path, sizeof(fullPath) - (len + 1));
    } else {
#ifdef DEBUG
      fprintf(stderr, "  assuming path does not YET exist\n");
#endif
      strlcpy(fullPath, path, sizeof(fullPath));
    }
  }
  
#ifdef DEBUG
  fprintf(stderr, "  resolved path to: %s\n", fullPath);
  fprintf(stderr, "\n");
#endif
  
  CFStringRef pathRef = CFStringCreateWithCString(kCFAllocatorDefault,
                                                  fullPath,
                                                  kCFStringEncodingUTF8);
  CFArrayAppendValue(config.paths, pathRef);
  CFRelease(pathRef);
}

// Parse commandline settings
static inline void parse_cli_settings(int argc, const char *argv[])
{
  // runtime os version detection
  SInt32 osMajorVersion, osMinorVersion;
  if (!(Gestalt(gestaltSystemVersionMajor, &osMajorVersion) == noErr)) {
    osMajorVersion = 0;
  }
  if (!(Gestalt(gestaltSystemVersionMinor, &osMinorVersion) == noErr)) {
    osMinorVersion = 0;
  }

  if ((osMajorVersion == 10) & (osMinorVersion < 5)) {
    fprintf(stderr, "The FSEvents API is unavailable on this version of macos!\n");
    exit(EXIT_FAILURE);
  }
  
  struct cli_info args_info;
  cli_parser_init(&args_info);
  
  if (cli_parser(argc, argv, &args_info) != 0) {
    exit(EXIT_FAILURE);
  }

  config.paths = CFArrayCreateMutable(NULL,
                                      (CFIndex)0,
                                      &kCFTypeArrayCallBacks);
  
  config.sinceWhen = args_info.since_when_arg;
  config.latency = args_info.latency_arg;
  config.format = args_info.format_arg;
  
  if (args_info.no_defer_flag)
    config.flags |= kFSEventStreamCreateFlagNoDefer;
  if (args_info.watch_root_flag)
    config.flags |= kFSEventStreamCreateFlagWatchRoot;
  
  if (args_info.ignore_self_flag) {
    if ((osMajorVersion == 10) & (osMinorVersion >= 6)) {
      config.flags |= kFSEventStreamCreateFlagIgnoreSelf;
    } else {
      fprintf(stderr, "MacOSX 10.6 or later is required for --ignore-self\n");
      exit(EXIT_FAILURE);
    }
  }
  
  if (args_info.file_events_flag) {
    if ((osMajorVersion == 10) & (osMinorVersion >= 7)) {
      config.flags |= kFSEventStreamCreateFlagFileEvents;
    } else {
      fprintf(stderr, "MacOSX 10.7 or later required for --file-events\n");
      exit(EXIT_FAILURE);
    }
  }
  
  if (args_info.inputs_num == 0) {
    append_path(".");
  } else {
    for (unsigned int i=0; i < args_info.inputs_num; ++i) {
      append_path(args_info.inputs[i]);
    } 
  }
  
  cli_parser_free(&args_info);
  
#ifdef DEBUG
  fprintf(stderr, "config.sinceWhen    %llu\n", config.sinceWhen);
  fprintf(stderr, "config.latency      %f\n", config.latency);

// STFU clang
#if __LP64__
  fprintf(stderr, "config.flags        %#.8x\n", config.flags);
#else
  fprintf(stderr, "config.flags        %#.8lx\n", config.flags);
#endif

  fprintf(stderr, "config.paths\n");
  
  long numpaths = CFArrayGetCount(config.paths);
  
  for (long i = 0; i < numpaths; i++) {
    char path[PATH_MAX];
    CFStringGetCString(CFArrayGetValueAtIndex(config.paths, i),
                       path,
                       PATH_MAX,
                       kCFStringEncodingUTF8);
    fprintf(stderr, "  %s\n", path);
  }
  
  fprintf(stderr, "\n");
#endif
}

// original output format for rb-fsevent
static void classic_output_format(size_t numEvents,
                                  char **paths)
{
  for (size_t i = 0; i < numEvents; i++) {
    fprintf(stdout, "%s:", paths[i]);
  }
  fprintf(stdout, "\n");
}

// output format used in the Yoshimasa Niwa branch of rb-fsevent
static void niw_output_format(size_t numEvents,
                              char **paths,
                              const FSEventStreamEventFlags eventFlags[],
                              const FSEventStreamEventId eventIds[])
{
  for (size_t i = 0; i < numEvents; i++) {
    fprintf(stdout, "%lu:%llu:%s\n",
            (unsigned long)eventFlags[i],
            (unsigned long long)eventIds[i],
            paths[i]);
  }
  fprintf(stdout, "\n");
}

static void callback(FSEventStreamRef streamRef,
                     void *clientCallBackInfo,
                     size_t numEvents,
                     void *eventPaths,
                     const FSEventStreamEventFlags eventFlags[],
                     const FSEventStreamEventId eventIds[])
{
  char **paths = eventPaths;

// commented out, at least for the moment, so that it doesn't inadvertently
// make reading formatted output painful. it might make sense to even make this
// its own output format.
//
//#ifdef DEBUG
//  fprintf(stderr, "\n");
//  fprintf(stderr, "FSEventStreamCallback fired!\n");
//  fprintf(stderr, "  numEvents: %lu\n", numEvents);
//  
//  for (size_t i = 0; i < numEvents; i++) {
//    fprintf(stderr, "  event path: %s\n", paths[i]);
//    fprintf(stderr, "  event flags: %#.8x\n", eventFlags[i]);
//    fprintf(stderr, "  event ID: %llu\n", eventIds[i]);
//  }
//  
//  fprintf(stderr, "\n");
//#endif
  
  if (config.format == kFSEventWatchOutputFormatClassic) {
    classic_output_format(numEvents, paths);
  } else if (config.format == kFSEventWatchOutputFormatNIW) {
    niw_output_format(numEvents, paths, eventFlags, eventIds);
  }
    
  fflush(stdout);
}

int main(int argc, const char *argv[])
{
  /*
   * a subprocess will initially inherit the process group of its parent. the
   * process group may have a control terminal associated with it, which would
   * be the first tty device opened by the group leader. typically the group
   * leader is your shell and the control terminal is your login device. a
   * subset of signals triggered on the control terminal are sent to all members
   * of the process group, in large part to facilitate sane and consistent
   * cleanup (ex: control terminal was closed).
   *
   * so why the overly descriptive lecture style comment?
   *   1. SIGINT and SIGQUIT are among the signals with this behavior
   *   2. a number of applications gank the above for their own use
   *   3. ruby's insanely useful "guard" is one of these applications
   *   4. despite having some level of understanding of POSIX signals and a few
   *      of the scenarios that might cause problems, i learned this one only
   *      after reading ruby 1.9's process.c
   *   5. if left completely undocumented, even slightly obscure bugfixes
   *      may be removed as cruft by a future maintainer
   *
   * hindsight is 20/20 addition: if you're single-threaded and blocking on IO
   * with a subprocess, then handlers for deferrable signals might not get run
   * when you expect them to. In the case of Ruby 1.8, that means making use of
   * IO::select, which will preserve correct signal handling behavior.
   */
  if (setpgid(0,0) < 0) {
    fprintf(stderr, "Unable to set new process group.\n");
    return 1;
  }
  
  parse_cli_settings(argc, argv);
  
  FSEventStreamContext context = {0, NULL, NULL, NULL, NULL};
  FSEventStreamRef stream;
  stream = FSEventStreamCreate(kCFAllocatorDefault,
                               (FSEventStreamCallback)&callback,
                               &context,
                               config.paths,
                               config.sinceWhen,
                               config.latency,
                               config.flags);
  
#ifdef DEBUG
  FSEventStreamShow(stream);
  fprintf(stderr, "\n");
#endif
  
  FSEventStreamScheduleWithRunLoop(stream,
                                   CFRunLoopGetCurrent(),
                                   kCFRunLoopDefaultMode);
  FSEventStreamStart(stream);
  CFRunLoopRun();
  FSEventStreamFlushSync(stream);
  FSEventStreamStop(stream);
  
  return 0;
}
