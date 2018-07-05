/* Copyright 2015-present Samsung Electronics Co., Ltd. and other contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "iotjs_def.h"
#include "iotjs_exception.h"
#include "iotjs_js.h"
#include "jerryscript-debugger.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char** environ;
#endif


static jerry_value_t WrapEval(const char* name, size_t name_len,
                              const char* source, size_t length) {
  static const char* args =
      "exports, require, module, native, __filename, __dirname";
  jerry_value_t res =
      jerry_parse_function((const jerry_char_t*)name, name_len,
                           (const jerry_char_t*)args, strlen(args),
                           (const jerry_char_t*)source, length, false);

  return res;
}


JS_FUNCTION(Compile) {
  DJS_CHECK_ARGS(2, string, string);

  iotjs_string_t file = JS_GET_ARG(0, string);
  iotjs_string_t source = JS_GET_ARG(1, string);

  const char* filename = iotjs_string_data(&file);
  const iotjs_environment_t* env = iotjs_environment_get();

  if (iotjs_environment_config(env)->debugger != NULL) {
    jerry_debugger_stop();
  }

  jerry_value_t jres =
      WrapEval(filename, strlen(filename), iotjs_string_data(&source),
               iotjs_string_size(&source));

  iotjs_string_destroy(&file);
  iotjs_string_destroy(&source);
  return jres;
}


JS_FUNCTION(CompileSnapshot) {
  DJS_CHECK_ARGS(1, string);

  iotjs_string_t path = JS_GET_ARG(0, string);
  const iotjs_environment_t* env = iotjs_environment_get();

  uv_fs_t fs_req;
  uv_fs_stat(iotjs_environment_loop(env), &fs_req, iotjs_string_data(&path),
             NULL);
  uv_fs_req_cleanup(&fs_req);

  if (!S_ISREG(fs_req.statbuf.st_mode)) {
    iotjs_string_destroy(&path);
    return JS_CREATE_ERROR(COMMON, "ReadSource error, not a regular file");
  }

  size_t size = 0;
  char* bytecode = iotjs__file_read(iotjs_string_data(&path), &size);
  if (bytecode == NULL || size == 0) {
    return JS_CREATE_ERROR(COMMON, "Could not load the snapshot source.");
  }

  return jerry_exec_snapshot((uint32_t*)bytecode, size, true);
}


// Callback function for DebuggerSourceCompile
static jerry_value_t wait_for_source_callback(
    const jerry_char_t* resource_name_p, size_t resource_name_size,
    const jerry_char_t* source_p, size_t size, void* data) {
  IOTJS_UNUSED(data);

  char* filename = (char*)resource_name_p;
  iotjs_string_t source =
      iotjs_string_create_with_buffer((char*)source_p, size);

  jerry_debugger_stop();

  return WrapEval(filename, resource_name_size, iotjs_string_data(&source),
                  iotjs_string_size(&source));
}


// Compile source received from debugger
JS_FUNCTION(DebuggerSourceCompile) {
  jerry_value_t res;
  jerry_debugger_wait_for_client_source(wait_for_source_callback, NULL, &res);
  return res;
}


JS_FUNCTION(CompileModule) {
  DJS_CHECK_ARGS(2, object, function);

  jerry_value_t jmodule = JS_GET_ARG(0, object);
  jerry_value_t jrequire = JS_GET_ARG(1, function);

  jerry_value_t jid = iotjs_jval_get_property(jmodule, "id");
  iotjs_string_t id = iotjs_jval_as_string(jid);
  jerry_release_value(jid);
  const char* name = iotjs_string_data(&id);

  int i = 0;
  while (js_modules[i].name != NULL) {
    if (!strcmp(js_modules[i].name, name)) {
      break;
    }

    i++;
  }

  jerry_value_t native_module_jval = iotjs_module_get(name);
  if (jerry_value_has_error_flag(native_module_jval)) {
    return native_module_jval;
  }

  jerry_value_t jexports = iotjs_jval_get_property(jmodule, "exports");
  jerry_value_t jres = jerry_create_undefined();

  if (js_modules[i].name != NULL) {
#ifdef ENABLE_SNAPSHOT
    jres = jerry_exec_snapshot_at((const void*)iotjs_js_modules_s,
                                  iotjs_js_modules_l, js_modules[i].idx, false);
#else
    jres = WrapEval(name, iotjs_string_size(&id),
                    (const char*)js_modules[i].code, js_modules[i].length);
#endif

    if (!jerry_value_has_error_flag(jres)) {
      jerry_value_t args[] = { jexports, jrequire, jmodule,
                               native_module_jval };

      jerry_value_t jfunc = jres;
      jres = jerry_call_function(jfunc, jerry_create_undefined(), args,
                                 sizeof(args) / sizeof(jerry_value_t));
      jerry_release_value(jfunc);
    }
  } else if (!jerry_value_is_undefined(native_module_jval)) {
    iotjs_jval_set_property_jval(jmodule, "exports", native_module_jval);
  } else {
    jres = iotjs_jval_create_error("Unknown native module");
  }

  jerry_release_value(jexports);
  iotjs_string_destroy(&id);
  return jres;
}


JS_FUNCTION(ReadSource) {
  DJS_CHECK_ARGS(1, string);

  iotjs_string_t path = JS_GET_ARG(0, string);
  const iotjs_environment_t* env = iotjs_environment_get();

  uv_fs_t fs_req;
  uv_fs_stat(iotjs_environment_loop(env), &fs_req, iotjs_string_data(&path),
             NULL);
  uv_fs_req_cleanup(&fs_req);

  if (!S_ISREG(fs_req.statbuf.st_mode)) {
    iotjs_string_destroy(&path);
    return JS_CREATE_ERROR(COMMON, "ReadSource error, not a regular file");
  }

  iotjs_string_t code = iotjs_file_read(iotjs_string_data(&path));
  jerry_value_t ret_val = iotjs_jval_create_string(&code);

  iotjs_string_destroy(&path);
  iotjs_string_destroy(&code);

  return ret_val;
}


JS_FUNCTION(Loadstat) {
  iotjs_environment_t* env = (iotjs_environment_t*)iotjs_environment_get();
  bool loadstat = iotjs_environment_config(env)->loadstat;
  return jerry_create_boolean(loadstat);
}


JS_FUNCTION(GetStackFrames) {
  uint32_t depth;

  if (jargc < 1 || jerry_value_is_undefined(jargv[0])) {
    depth = 10;
  } else if (!jerry_value_is_number(jargv[0])) {
    return JS_CREATE_ERROR(COMMON, "argument must be an integer.");
  } else {
    depth = jerry_get_number_value(jargv[0]);
  }

  // create frames
  uint32_t* frames = malloc(sizeof(uint32_t) * depth);
  memset(frames, 0, sizeof(uint32_t) * depth);
  jerry_get_backtrace_depth(frames, depth);

  jerry_value_t jframes = jerry_create_array(depth);
  for (uint32_t i = 0; i < depth; ++i) {
    jerry_set_property_by_index(jframes, i, jerry_create_number(frames[i]));
  }

  free(frames);
  return jframes;
}


JS_FUNCTION(ReadParserDump) {
  int pos = JS_GET_ARG(0, number);
  return jerry_read_parser_dump(pos);
}


JS_FUNCTION(Umask) {
  uint32_t old;

  if (jargc < 1 || jerry_value_is_undefined(jargv[0])) {
    old = umask(0);
    umask((mode_t)old);
  } else if (!jerry_value_is_number(jargv[0])) {
    return JS_CREATE_ERROR(COMMON, "argument must be an integer.");
  } else {
    int oct;
    oct = (int)jerry_get_number_value(jargv[0]);
    old = umask((mode_t)oct);
  }
  return jerry_create_number(old);
}

JS_FUNCTION(Cwd) {
  char path[IOTJS_MAX_PATH_SIZE];
  size_t size_path = sizeof(path);
  int err = uv_cwd(path, &size_path);
  if (err) {
    return JS_CREATE_ERROR(COMMON, "cwd error");
  }

  return jerry_create_string_from_utf8((const jerry_char_t*)path);
}

JS_FUNCTION(Chdir) {
  DJS_CHECK_ARGS(1, string);

  iotjs_string_t path = JS_GET_ARG(0, string);
  int err = uv_chdir(iotjs_string_data(&path));

  if (err) {
    iotjs_string_destroy(&path);
    return JS_CREATE_ERROR(COMMON, "chdir error");
  }

  iotjs_string_destroy(&path);
  return jerry_create_undefined();
}


JS_FUNCTION(DoExit) {
  iotjs_environment_t* env = iotjs_environment_get();

  if (!iotjs_environment_is_exiting(env)) {
    DJS_CHECK_ARGS(1, number);
    int exit_code = JS_GET_ARG(0, number);

    iotjs_set_process_exitcode(exit_code);
    iotjs_environment_go_state_exiting(env);
  }
  return jerry_create_undefined();
}

JS_FUNCTION(Kill) {
  DJS_CHECK_ARGS(1, number);
  int signal = JS_GET_ARG(0, number);
  kill(getpid(), signal);
  return jerry_create_undefined();
}

#define NANOS_PER_SEC 1000000000
JS_FUNCTION(Hrtime) {
  uint64_t t = uv_hrtime();
  uint32_t n1 = (t / NANOS_PER_SEC) >> 32;
  uint32_t n2 = (t / NANOS_PER_SEC) & 0xffffffff;
  uint32_t n3 = t % NANOS_PER_SEC;

  jerry_value_t out = jerry_create_array(2);
  jerry_value_t left = jerry_create_number(n1 * 0x100000000 + n2);
  jerry_value_t right = jerry_create_number(n3);

  iotjs_jval_set_property_by_index(out, 0, left);
  iotjs_jval_set_property_by_index(out, 1, right);

  jerry_release_value(left);
  jerry_release_value(right);
  return out;
}


JS_FUNCTION(GetEnvironArray) {
  uint32_t size = 0;
  while (environ[size])
    size++;

  jerry_value_t envarr = jerry_create_array(size);
  for (uint32_t i = 0; i < size; i++) {
    jerry_value_t val = jerry_create_string((jerry_char_t*)environ[i]);
    jerry_set_property_by_index(envarr, i, val);
    jerry_release_value(val);
  }
  return envarr;
}

JS_FUNCTION(SetEnviron) {
  iotjs_string_t key = JS_GET_ARG(0, string);
  iotjs_string_t val = JS_GET_ARG(1, string);

  setenv(iotjs_string_data(&key), iotjs_string_data(&val), 1);

  iotjs_string_destroy(&key);
  iotjs_string_destroy(&val);
  return jerry_create_undefined();
}

JS_FUNCTION(CreateUVException) {
  int uv_errno = JS_GET_ARG(0, number);
  iotjs_string_t syscall = JS_GET_ARG(1, string);

  jerry_value_t err =
      iotjs_create_uv_exception(uv_errno, iotjs_string_data(&syscall));
  iotjs_string_destroy(&syscall);
  return err;
}

JS_FUNCTION(ForceGC) {
  jerry_gc();
  return jerry_create_boolean(true);
}

JS_FUNCTION(DLOpen) {
  iotjs_string_t location = JS_GET_ARG(0, string);
  void (*initfn)(jerry_value_t);

  void* handle = dlopen(iotjs_string_data(&location), RTLD_LAZY);
  if (handle == NULL) {
    fprintf(stderr, "dlopen: error(%s)\n", dlerror());
    return jerry_create_number(-1);
  }

  initfn = dlsym(handle, "iotjs_module_register");
  // check for dlsym
  char* error = dlerror();
  if (error != NULL) {
    fprintf(stderr, "dlsym: error(%s)\n", error);
    dlclose(handle);
    return jerry_create_number(-1);
  }

  jerry_value_t exports = jerry_create_object();
  (*initfn)(exports);
  return exports;
}

JS_FUNCTION(MemoryUsage) {
  size_t rss;
  jerry_heap_stats_t stats;
  memset(&stats, 0, sizeof(jerry_heap_stats_t));
  int err = uv_resident_set_memory(&rss);
  if (err) {
    char errStr[64];
    sprintf(errStr, "uv_resident_set_memory error with code %d", err);
    return JS_CREATE_ERROR(COMMON, errStr);
  }
  jerry_get_memory_stats(&stats);
  jerry_value_t ret = jerry_create_object();
  iotjs_jval_set_property_number(ret, "rss", rss);
  iotjs_jval_set_property_number(ret, "peakHeapTotal",
                                 stats.peak_allocated_bytes);
  iotjs_jval_set_property_number(ret, "heapTotal", stats.size);
  iotjs_jval_set_property_number(ret, "heapUsed", stats.allocated_bytes);
  // FIXME external memory usage is not implement yet
  // iotjs_jval_set_property_number(ret, "external", -1);
  return ret;
}

void SetNativeSources(jerry_value_t native_sources) {
  for (int i = 0; js_modules[i].name; i++) {
    iotjs_jval_set_property_jval(native_sources, js_modules[i].name,
                                 jerry_create_boolean(true));
  }
}

static void SetProcessEnv(jerry_value_t process) {
  const char *homedir, *iotjspath, *iotjsenv;
  homedir = getenv("HOME");
  if (homedir == NULL) {
    homedir = "";
  }

  iotjspath = getenv("IOTJS_PATH");
  if (iotjspath == NULL) {
#if defined(__NUTTX__) || defined(__TIZENRT__)
    iotjspath = "/mnt/sdcard";
#else
    iotjspath = "";
#endif
  }

#if defined(EXPERIMENTAL)
  iotjsenv = "experimental";
#else
  iotjsenv = "";
#endif

  jerry_value_t env = jerry_create_object();
  iotjs_jval_set_property_string_raw(env, IOTJS_MAGIC_STRING_HOME_U, homedir);
  iotjs_jval_set_property_string_raw(env, IOTJS_MAGIC_STRING_IOTJS_PATH_U,
                                     iotjspath);
  iotjs_jval_set_property_string_raw(env, IOTJS_MAGIC_STRING_IOTJS_ENV_U,
                                     iotjsenv);

  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_ENV, env);

  jerry_release_value(env);
}


static void SetProcessIotjs(jerry_value_t process) {
  // IoT.js specific
  jerry_value_t iotjs = jerry_create_object();
  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_IOTJS, iotjs);

  iotjs_jval_set_property_string_raw(iotjs, IOTJS_MAGIC_STRING_BOARD,
                                     TOSTRING(TARGET_BOARD));
  jerry_release_value(iotjs);
}


static void SetProcessArgv(jerry_value_t process) {
  const iotjs_environment_t* env = iotjs_environment_get();
  uint32_t argc = iotjs_environment_argc(env);

  jerry_value_t argv = jerry_create_array(argc);

  for (uint32_t i = 0; i < argc; ++i) {
    const char* argvi = iotjs_environment_argv(env, i);
    jerry_value_t arg = jerry_create_string((const jerry_char_t*)argvi);
    iotjs_jval_set_property_by_index(argv, i, arg);
    jerry_release_value(arg);
  }
  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_ARGV, argv);

  jerry_release_value(argv);
}


static void SetProcessExecArgv(jerry_value_t process) {
  jerry_value_t execArgv = jerry_create_array(0);
  iotjs_jval_set_property_jval(process, "execArgv", execArgv);
  jerry_release_value(execArgv);
}


static void SetProcessExecPath(jerry_value_t process) {
  size_t size = 2 * PATH_MAX;
  char* exec_path = malloc(size);
  if (exec_path == NULL) {
    // FIXME(Yorkie): OOM to be fixed
    fprintf(stderr, "Out of Memory\n");
    return;
  }
  if (uv_exepath(exec_path, &size) == 0) {
    iotjs_jval_set_property_string_raw(process, "execPath", exec_path);
  } else {
    // FIXME(Yorkie): set from argv[0]
    iotjs_jval_set_property_string_raw(process, "execPath", "");
  }
  free(exec_path);
}


static void SetBuiltinModules(jerry_value_t builtin_modules) {
  for (unsigned i = 0; js_modules[i].name; i++) {
    iotjs_jval_set_property_jval(builtin_modules, js_modules[i].name,
                                 jerry_create_boolean(true));
  }
  for (unsigned i = 0; i < iotjs_modules_count; i++) {
    iotjs_jval_set_property_jval(builtin_modules, iotjs_modules[i].name,
                                 jerry_create_boolean(true));
  }
}


jerry_value_t InitProcess() {
  jerry_value_t process = jerry_create_object();

  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_COMPILE, Compile);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_COMPILEMODULE,
                        CompileModule);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_READSOURCE, ReadSource);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_CWD, Cwd);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_CHDIR, Chdir);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_UMASK, Umask);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_DEBUGGERSOURCECOMPILE,
                        DebuggerSourceCompile);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_DOEXIT, DoExit);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_KILL, Kill);
  iotjs_jval_set_method(process, "hrtime", Hrtime);

  // env
  iotjs_jval_set_method(process, "_getEnvironArray", GetEnvironArray);
  iotjs_jval_set_method(process, "_setEnviron", SetEnviron);
  iotjs_jval_set_method(process, "_loadstat", Loadstat);
  SetProcessEnv(process);

  // errors
  iotjs_jval_set_method(process, "_createUVException", CreateUVException);
  iotjs_jval_set_method(process, "_getStackFrames", GetStackFrames);
  iotjs_jval_set_method(process, "_readParserDump", ReadParserDump);

  // virtual machine
  iotjs_jval_set_method(process, "gc", ForceGC);

  // native module
  iotjs_jval_set_method(process, "dlopen", DLOpen);

  // snapshot
  iotjs_jval_set_method(process, "compileSnapshot", CompileSnapshot);

  // stats
  iotjs_jval_set_method(process, "memoryUsage", MemoryUsage);

  // process.builtin_modules
  jerry_value_t builtin_modules = jerry_create_object();
  SetBuiltinModules(builtin_modules);
  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_BUILTIN_MODULES,
                               builtin_modules);
  jerry_release_value(builtin_modules);

  // process.pid
  iotjs_jval_set_property_number(process, IOTJS_MAGIC_STRING_PID,
                                 (double)getpid());

  // process.platform
  iotjs_jval_set_property_string_raw(process, IOTJS_MAGIC_STRING_PLATFORM,
                                     TARGET_OS);

  // process.arch
  iotjs_jval_set_property_string_raw(process, IOTJS_MAGIC_STRING_ARCH,
                                     TARGET_ARCH);

  // process.version
  iotjs_jval_set_property_string_raw(process, IOTJS_MAGIC_STRING_VERSION,
                                     IOTJS_VERSION);


  // Set iotjs
  SetProcessIotjs(process);
  bool wait_source;
  if (iotjs_environment_config(iotjs_environment_get())->debugger != NULL) {
    wait_source = iotjs_environment_config(iotjs_environment_get())
                      ->debugger->wait_source;
  } else {
    wait_source = false;
  }

  if (!wait_source) {
    SetProcessArgv(process);
    SetProcessExecArgv(process);
    SetProcessExecPath(process);
  }

  jerry_value_t wait_source_val = jerry_create_boolean(wait_source);
  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_DEBUGGERWAITSOURCE,
                               wait_source_val);
  jerry_release_value(wait_source_val);

  return process;
}
