/*
 *  COMMAND.COM-compatible command processor for DOS.
 *
 *  Copyright (C) 1997, CENTROID CORPORATION, HOWARD, PA 16841
 *  Copyright (C) Allen S. Cheung (allencheung@fastmail.ca)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * comcom32 project note:
 * The use of GPLv3+ license for this code was confirmed by Centroid Corp here:
 * https://github.com/stsp/comcom32/issues/12#issuecomment-472004939
 *
 * See also original copyrights below.
 */

/*
*   FILE NAME:
*       COMMAND.C
*
*   PROGRAMMER(S):
*       Allen S. Cheung (allencheung@fastmail.ca)
*
*   UPDATE:
*       15-Apr-2002
*
*   LICENSE:
*       GNU General Public License
*
*   HOW TO COMPILE AND USE WITH FREEDOS:
*       1) Install DJGPP on your development computer.
*       2) Install FreeDos on your target FreeDos computer.
*       3a) Compile the program with the accompanying make file.
*           type: make -f Makefile
*       3b) OR, use rhide to build the program with the accompanying
*           project file (command.gpr).
*       4) Rename the resultant program from COMMAND.EXE to COMMAND.COM.
*       5) Copy COMMAND.COM to the root directory
*          of the boot drive of your target FreeDos computer.
*       6) Copy CWSDPMI.EXE (from DJGPP distro) to the root directory
*          of the boot drive of your target FreeDos computer.
*       7) Reboot said computer.
*
*   BUGS, PECULIARITIES, AND/OR UNIMPLEMENTED FEATURES
*       - Some commands are incomplete: TIME, DATE
*       - Some built-in commands should really be separate executables
*         and not built-in. They are: CHOICE, MORE, PAUSE, XCOPY.
*
*   COPYRIGHT (C) 1997  CENTROID CORPORATION, HOWARD, PA 16841
*
***/

/* WARNING:	This is not the original version.
 *			modified for FreeDOS-32 by Salvo Isaja and Hanzac Chen
 */

/* DOS environment and /E support by Stas Sergeev
 */

#include <dos.h>
#include <time.h>
#include <glob.h>
#include <utime.h>
#include <conio.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#include <stubinfo.h>
#include <process.h>
#include <sys/movedata.h>	/* for movedata and dosmemget */
#include <sys/segments.h>
#include <go32.h>
#include <sys/farptr.h>

#include "cmdbuf.h"
#include "memmem.h"
#include "version.h"
#include "command.h"

/*
 * These declarations/definitions turn off some unwanted DJGPP features
 */

#ifdef __DJGPP__
#include <crt0.h>
#include <dpmi.h>
extern char **environ;

#define UNUSED __attribute__((unused))
int _crt0_startup_flags =
       _CRT0_FLAG_USE_DOS_SLASHES |          // keep the backslashes
       _CRT0_FLAG_DISALLOW_RESPONSE_FILES |  // no response files (i.e. `@gcc.rf')
       _CRT0_FLAG_NO_LFN |                   // disable long file names
       _CRT0_FLAG_LOCK_MEMORY |              // disable virtual memory
       _CRT0_FLAG_PRESERVE_FILENAME_CASE;    // keep DOS names uppercase
char **__crt0_glob_function(char *_argument UNUSED) {return NULL;} // prevent wildcard expansion of arguments of main()
void __crt0_load_environment_file(char *_app_name UNUSED) {} // prevent loading of environment file
#endif

static int shell_mode = SHELL_NORMAL;
static int shell_permanent;

/* define to sync RM/PM env data - consumes more memory */
#define SYNC_ENV 0

/*
 * Command parser defines/variables
 */
static char cmd_line[MAX_CMD_BUFLEN] = ""; // when this string is not "" it triggers command execution
static char cmd[MAX_CMD_BUFLEN] = "";
static char cmd_arg[MAX_CMD_BUFLEN] = "";
static char cmd_switch[MAX_CMD_BUFLEN] = "";
static char cmd_args[MAX_CMD_BUFLEN] = "";
static char goto_label[MAX_CMD_BUFLEN] = "";

/*
 * Pipe variables and defines
 */
static char pipe_file[2][MAX_CMD_BUFLEN] = {"",""};
static int pipe_file_redir_count[2];
static char pipe_to_cmd[MAX_CMD_BUFLEN] = "";
static int pipe_to_cmd_redir_count;

/*
 * Command interpreter/executor defines/variables
 */
#define MAX_STACK_LEVEL        20 // Max number of batch file call stack levels
#define MAX_BAT_ARGS           32 // Max number of batch file arguments

static int need_to_crlf_at_next_prompt;
static int stack_level = 0;
static int echo_on[MAX_STACK_LEVEL];
static char bat_file_path[MAX_STACK_LEVEL][FILENAME_MAX];  // when this string is not "" it triggers batch file execution
static char bat_arg[MAX_STACK_LEVEL][MAX_BAT_ARGS][MAX_CMD_BUFLEN];
static int bat_file_line_number[MAX_STACK_LEVEL];
static char pushd_stack[MAX_STACK_LEVEL][MAXPATH];
static int pushd_stack_level = 0;
static unsigned error_level = 0;  // Program execution return code
static char for_var;
static const char *for_val;
static int exiting;

/*
 * File attribute constants
 */
static const char attrib_letters[4] = {'R', 'A', 'S', 'H'};
static const unsigned attrib_values[4] = {_A_RDONLY, _A_ARCH, _A_SYSTEM, _A_HIDDEN};

/*
 * Some private prototypes
 */
static void parse_cmd_line(void);
static void perform_external_cmd(int call, char *ext_cmd);
static void exec_cmd(void);
static void perform_set(const char *arg);
static void list_cmds(void);
//static void perform_unimplemented_cmd(void);

static int installable_command_check(const char *cmd, const char *tail)
{
  /* from RBIL

  AX = AE00h
  DX = magic value FFFFh
  CH = FFh
  CL = length of command line tail (4DOS v4.0)
  DS:BX -> command line buffer (see #02977)
  DS:SI -> command name buffer (see #02978)
  DI = 0000h (4DOS v4.0)

  Return:
  AL = FFh if this command is a TSR extension to COMMAND.COM
  AL = 00h if the command should be executed as usual


  Format of COMMAND.COM command line buffer:

  Offset  Size    Description     (Table 02977)
  00h    BYTE    max length of command line, as in INT 21/AH=0Ah
  01h    BYTE    count of bytes to follow, excluding terminating 0Dh
  N BYTEs   command line text, terminated by 0Dh


  Format of command name buffer:

  Offset  Size    Description     (Table 02978)
  00h    BYTE    length of command name
  01h  N BYTEs   uppercased command name (blank-padded to 11 chars by 4DOS v4)

  */

  const char *p;
  char *q;
  int i;
  const char *name;
  int tlen;
  int nlen;
  uint16_t ax;

  struct {
    uint8_t nlen;
    char nbuf[11];

    uint8_t cmax;
    uint8_t clen;
    char cbuf[256];
  } __attribute__((packed)) s;

  p = strrchr(cmd, '\\');
  if (p)
    name = p + 1;
  else
    name = cmd;

  nlen = 0;
  for (p = name, q = &s.nbuf[0], i = 0; *p; p++) {
    if (*p == '.') {
      nlen = i;
      if (i < 8) {
        memset(q + i, ' ', 8 - i);
        i = 8;
      }
      continue;
    }
    if (i >= sizeof(s.nbuf))
      return 0;
    q[i++] = toupper(*p);
  }
  if (i < 11)
    memset(q + i, ' ', 11 - i);
  if (!nlen)        // no dot found
    nlen = i;
  s.nlen = nlen;    // does not cover extension

  if (strlen(cmd) + strlen(tail) + 2 >= sizeof(s.cbuf))
    return 0;
  s.cmax = sizeof(s.cbuf) - 1;
  if (tail[0]) {
    s.clen = snprintf(s.cbuf, sizeof(s.cbuf), "%s %s\r", cmd, tail) - 1;
    tlen = strlen(tail) + 1;  // account for 'space'
  } else {
    s.clen = snprintf(s.cbuf, sizeof(s.cbuf), "%s\r", cmd) - 1;
    tlen = 0;
  }

  asm volatile ("int $0x2f\n"
    : "=a"(ax)
    : "a"(0xae00),
      "c"(0xff00 + tlen),
      "d"(0xffff),
      "b"(&s.cmax),
      "S"(&s.nlen),
      "D"(0)
    : "cc", "memory" /* "memory" is needed even for any pointer read! */);

  return ((ax & 0xff) == 0xff);
}

/***
*
*   FUNCTION:    conv_unix_path_to_ms_dos
*
*   PROGRAMMER:  Allen S. Cheung
*
*   UPDATE:      04-Jan-2001
*
*   PURPOSE:     Force the given filepath into MS-DOS style
*
*   CALL:        void conv_unix_path_to_ms_dos(char *path)
*
*   WHERE:       *path is a file path either in Unix or MS-DOS style,
*                    which will be converted to MS-dos style if
*                    it was in Unix format.  (I/R)
*
*   RETURN:       none
*
*   NOTES:
*
***/
static void conv_unix_path_to_ms_dos(char *path)
  {
  char *p = path;
  if (p != NULL)
    {
    while (*p != '\0')
      {
      if (*p == '/') *p = '\\'; // change slashes to backslashes
      /* *p = toupper(*p); change to uppercase */
      p++;
      }
    }
  }

static int is_drive_spec(char *s)    // check for form "A:"
  {
  if (!isalpha(s[0]))
    return false;
  if (s[1] != ':')
    return false;
  if (s[2] != '\0')
    return false;
  return true;
  }

static int is_drive_spec_with_slash(char *s)  // check for form "C:\"
  {
  if (!isalpha(s[0]))
    return false;
  if (s[1] != ':')
    return false;
  if (s[2] != '\\')
    return false;
  if (s[3] != '\0')
    return false;
  return true;
  }

static int has_trailing_slash(char *s)
  {
  if (*s == '\0')
    return false;
  s = strchr(s,'\0')-1;
  if (*s == '\\' || *s == '/')
    return true;
  return false;
  }

static int has_wildcard(char *s)
  {
  if (strchr(s, '*') != NULL)
    return true;
  if (strchr(s, '?') != NULL)
    return true;
  return false;
  }

static void reset_batfile_call_stack(void)
  {
  static int first_time = true;
  int ba;

  if (!first_time)
    {
    if (bat_file_path[stack_level][0] != '\0')
      {
      cprintf("Batch file aborted - %s, line %d\r\n",
        bat_file_path[stack_level], bat_file_line_number[stack_level]);
      }
    }
  first_time = false;
  // initialize stack
  for (stack_level = 0; stack_level < MAX_STACK_LEVEL; stack_level++)
    {
    bat_file_path[stack_level][0] = '\0';
    for (ba = 0; ba < MAX_BAT_ARGS; ba++)
      bat_arg[stack_level][ba][0] = '\0';
    bat_file_line_number[stack_level] = 0;
    echo_on[stack_level] = true;
    }
  stack_level = 0;
  }

static void output_prompt(void)
  {
  char cur_drive_and_path[MAXPATH];
  const char *promptvar = getenv("PROMPT");

  if (need_to_crlf_at_next_prompt)
    {
    cputs("\r\n");
    need_to_crlf_at_next_prompt = false;
    }

  if (promptvar == NULL)
    promptvar = "$p$g";
  getcwd(cur_drive_and_path, MAXPATH);
  /* The disk letter is changed to upper-case */
  cur_drive_and_path[0] = toupper(cur_drive_and_path[0]);
  conv_unix_path_to_ms_dos(cur_drive_and_path);
  while (*promptvar != '\0')
    {
    if (*promptvar == '$')
      {
      promptvar++;
      switch (toupper(*promptvar))
        {
        case '\0':
          promptvar--;
          break;
        case 'Q': //    = (equal sign)
          putch('=');
          break;
        case '$': //    $ (dollar sign)
          putch('$');
          break;
        case 'T': //    Current time (TODO: emulate centisecond)
          {
          time_t t = time(NULL);
          struct tm *loctime = localtime (&t);
          cprintf("%2d:%02d:%02d", loctime->tm_hour, loctime->tm_min, loctime->tm_sec);
          break;
          }
        case 'D': //    Current date
          {
          time_t t = time(NULL);
          struct tm *loctime = localtime (&t);
          cprintf("%02d-%02d-%04d", loctime->tm_mon+1, loctime->tm_mday, loctime->tm_year+1900);
          break;
          }
        case 'P': //   Current drive and path
          {
          cputs(cur_drive_and_path);
          break;
          }
        case 'N': //    Current drive
          {
          putch(*cur_drive_and_path);
          putch(':');
          break;
          }
        case 'G': //    > (greater-than sign)
          putch('>');
          break;
        case 'L': //    < (less-than sign)
          putch('<');
          break;
        case 'B': //    | (pipe)
          putch('|');
          break;
        case '_': //    ENTER-LINEFEED
          cputs("\r\n");
          break;
        case 'E': //    ASCII escape code
          putch(27);
          break;
        case 'H': //    Backspace
          putch(8);
          break;
        default:
          putch('$');
          putch(*promptvar);
          break;
        }
      }
    else
      putch(*promptvar);
    promptvar++;
    }
  }

static void extract_args(char *src)
  {
  char *dest, *saved_src = src;

  // scout ahead to see if there are really any arguments
  while (*src == ' ' || *src == '\t')
    src++;
  if (*src == '\0')
    {
    cmd_arg[0] = '\0';
    cmd_switch[0] = '\0';
    cmd_args[0] = '\0';
    return;
    }

  // extract combined arguments
  src = saved_src;
  if (*src == ' ' || *src == '\t')
    src++;
  memmove(cmd_args, src, strlen(src)+1);

  // extract first occurring single argument
  src = cmd_args;
  while (*src == ' ' || *src == '\t')
    src++;
  dest = cmd_arg;
  while (*src != ' ' && *src != '\t' && *src != '\0')
    {
    *dest = *src;
    dest++;
    src++;
    if (*src == '/')
      break;
    }
  *dest = '\0';

  // copy the single argument to cmd_switch if it qualifies as a switch
  if (cmd_arg[0] == '/')
    strcpy(cmd_switch, cmd_arg);
  else
    cmd_switch[0] = '\0';
  return;
  }

static void advance_cmd_arg(void)
  {
  char *extr;

  extr = cmd_args;

  // skip over first argument
  while (*extr == ' ' || *extr == '\t')
    extr++;
  if (*extr == '\0')
    goto NoArgs;

  while (*extr != ' ' && *extr != '\t' && *extr != '\0')
    {
    extr++;
    if (*extr == '/')
      break;
    }
  if (*extr == '\0')
    goto NoArgs;

  // extract the rest
  extract_args(extr);
  return;

NoArgs:
  cmd_arg[0] = '\0';
  cmd_switch[0] = '\0';
  cmd_args[0] = '\0';
  return;
  }

static unsigned short keyb_shift_states;
static unsigned short keyb_get_rawcode(void)
{
  unsigned short c = getch();

  if (c == 0x00/* || c == 0xE0*/)
    c = getch()<<8;

  if (c == KEY_INSERT)
    keyb_shift_states ^= KEYB_FLAG_INSERT;

  return c;
}
static unsigned short keyb_get_shift_states(void)
{
  return keyb_shift_states;
}

static void prompt_for_and_get_cmd(void)
  {
  int flag = 0, key = 0, len;
  char conbuf[MAX_CMD_BUFLEN+1];

  output_prompt();
  /* Console initialize */
  flag = keyb_get_shift_states();
  if (!(flag&KEYB_FLAG_INSERT))
    _setcursortype(_NORMALCURSOR);
  else
    _setcursortype(_SOLIDCURSOR);

  do {
    /* Wait and get raw key code */
    key = keyb_get_rawcode();
    flag = keyb_get_shift_states();

//    if (KEY_ASCII(key) == KEY_EXT)
//      key = KEY_EXTM(key);
//    else
    if (KEY_ASCII(key) != 0)
      key = KEY_ASCII(key);
    switch (key)
    {
      case 0:
        break;
      case 3:
      case 0x100:
        cmdbuf_clear(conbuf);
        break;
      case KEY_ENTER:
        break;
      case KEY_BACKSPACE:
        if (cmdbuf_move(conbuf, LEFT))
        {
          /* Delete the character at the end of string */
          cmdbuf_delch(conbuf);
        }
        break;
      case KEY_DELETE:
        cmdbuf_delch(conbuf);
        break;
      case KEY_INSERT:
        if (!(flag&KEYB_FLAG_INSERT))
          _setcursortype(_NORMALCURSOR);
        else
          _setcursortype(_SOLIDCURSOR);
        break;
      case KEY_UP:
        cmdbuf_move(conbuf, UP);
        break;
      case KEY_LEFT:
        cmdbuf_move(conbuf, LEFT);
        break;
      case KEY_RIGHT:
        cmdbuf_move(conbuf, RIGHT);
        break;
      case KEY_DOWN:
        cmdbuf_move(conbuf, DOWN);
        break;
      case KEY_HOME:
        cmdbuf_move(conbuf, HOME);
        break;
      case KEY_END:
        cmdbuf_move(conbuf, END);
        break;
      default:
        if (KEY_ASCII(key) != 0x00/* && KEY_ASCII(key) != 0xE0*/) {
          char c = cmdbuf_putch(conbuf, MAX_CMD_BUFLEN-2, KEY_ASCII(key), flag);
          if (c)
            putch(c);
        }
        break;
    }
  } while (key != KEY_ENTER);

  len = cmdbuf_get_tail();
  /* Get the size of typed string */
  strncpy(cmd_line, (char *)cmdbuf_gets(conbuf), len);

  cmd_line[len] = '\0';
  parse_cmd_line();
  cputs("\r\n");
  }

static int get_choice(const char *choices)
  {
  int choice, key;
//  strupr(choices);
  do
    {
    key = getch();
    if (key == 0)
      continue;
    } while (strchr(choices, toupper(key)) == NULL);
  choice = toupper(key);
  cprintf("%c", choice);
  cputs("\r\n");
  return choice;
  }

static void get_cmd_from_bat_file(void)
  {
  FILE *cmd_file;
  int line_num, c, ba;
  char *s;

  if (bat_file_line_number[stack_level] != MAXINT)
    bat_file_line_number[stack_level]++;

  cmd_file = fopen(bat_file_path[stack_level], "rt");
  if (cmd_file == NULL)
    {
    cprintf("Cannot open %s\r\n", bat_file_path[stack_level]);
    goto ErrorDone;
    }

  for (line_num = 0; line_num < bat_file_line_number[stack_level]; line_num++)
    {
    /* input as much of the line as the buffer can hold */
    s = fgets(cmd_line, MAX_CMD_BUFLEN, cmd_file);

    /* if s is null, investigate why */
    if (s == NULL)
      {
      /* check for error */
      if (ferror(cmd_file))
        {
        cprintf("Read error: %s, line %d\r\n", bat_file_path[stack_level], line_num+1);
        goto ErrorDone;
        }
      /* line is unavailable because of end of file */
      if (goto_label[0] != '\0')
        {
        cprintf("Label not found - %s\r\n", goto_label);
        goto_label[0] = '\0';
        goto ErrorDone;
        }
      goto FileDone;
      }

    /*
    *   Check for newline character;
    *   If present, we have successfully reached end of line
    *   If not present, continue getting line until newline or eof encountered
    */
    s = strchr(cmd_line, '\n');
    if (s != NULL)
      *s = '\0';
    else
      {
      do
        {
        c = fgetc(cmd_file);
        } while (c != '\n' && c != EOF);  // if eof occurs here, it needs
                                         // to be caught on the next iteration
                                         // but not now, and not here.
      if (ferror(cmd_file))
        {
        cprintf("Read error: %s, line %d\r\n", bat_file_path[stack_level], line_num+1);
        goto ErrorDone;
        }
      }

    // check for goto arrival at labeled destination
    if (goto_label[0] != '\0')
      {
      s = cmd_line;
      while (*s == ' ' || *s == '\t')
        s++;
      if (*s == ':')
        {
        s++;
        if (strnicmp(goto_label, s, strlen(goto_label)) == 0)
          {
          s += strlen(goto_label);
          if (*s == ' ' || *s == '\t' || *s == '\0')
            {
            // we have arrived... set line number, erase goto label
            bat_file_line_number[stack_level] = line_num + 1;
            goto_label[0] = '\0';
            break;
            }
          }
        }
      }
    }

  // parse command
  parse_cmd_line();

  // deal with echo on/off and '@' at the beginning of the command line
  if (cmd[0] == '@')
    memmove(cmd, cmd+1, strlen(cmd));
  else
    {
    if (echo_on[stack_level])
      {
      output_prompt();
      cputs(cmd_line);
      cputs("\r\n");
      }
    }

  goto RoutineDone;

ErrorDone:
  reset_batfile_call_stack();
FileDone:
  cmd_line[0] = '\0';
  parse_cmd_line();  // this clears cmd[], cmd_arg[], cmd_switch[], and cmd_args[]
  bat_file_path[stack_level][0] = '\0';
  for (ba = 0; ba < MAX_BAT_ARGS; ba++)
    bat_arg[stack_level][ba][0] = '\0';
  bat_file_line_number[stack_level] = 0;
  echo_on[stack_level] = true;
  if (stack_level > 0)
    stack_level--;
RoutineDone:
  if (cmd_file != NULL)
    fclose(cmd_file);
  }

static int ensure_dir_existence(char *dir)
  {
  char *c;
  char dir_path[MAXPATH];

  strcpy(dir_path, dir);
  if (*(strchr(dir_path, '\0')-1) == '\\')     // take away ending backslash
    *(strchr(dir_path, '\0')-1) = '\0';

  if (file_access(dir_path, D_OK) != 0)
    {
    c = strchr(dir_path, '\\');
    while (c != NULL)
      {
      c = strchr(c+1, '\\');
      if (c == NULL)
        printf("Creating directory - %s\\\n", dir_path);
      else
        *c = '\0';
      if (mkdir(dir_path) != 0 && c == NULL)
        {
        cprintf("Unable to create directory - %s\\\r\n", dir_path);
        return -1;
        }
      if (c != NULL)
        *c = '\\';
      }
    }
  return 0;
  }

static int copy_single_file(char *source_file, char *dest_file, int transfer_type)
  {
  FILE *source_stream;
  FILE *dest_stream;
  char transfer_buffer[32768];
  size_t byte_count;

  if (stricmp(source_file, dest_file) == 0)
    {
    cprintf("Source and destination cannot match - %s\r\n", source_file);
    return -1;
    }

  /* Open file for copy */
  source_stream = fopen(source_file, "rb");
  if (source_stream == NULL)
    {
    cprintf("Unable to open source file - %s\r\n", source_file);
    return -1;
    }
  dest_stream = fopen(dest_file, "wb");
  if (dest_stream == NULL)
    {
    cprintf("Unable to open destination file - %s\r\n", dest_file);
    fclose(source_stream);
    return -1;
    }

  /* Copy file contents*/
  do
    {
    byte_count = fread(transfer_buffer, 1, 32768, source_stream);
    if (byte_count > 0)
      {
      if (fwrite(transfer_buffer, 1, byte_count, dest_stream) != byte_count)
        goto copy_error_close;
      }
    }
  while (byte_count > 0);

  /* Copy date and time */
  fflush(dest_stream);
  if (file_copytime (fileno(dest_stream), fileno(source_stream)) != 0)
    goto copy_error_close;

  /* Close source and dest files */
  fclose(source_stream);
  if (fclose(dest_stream) != 0)
    goto copy_error;
  return 0;

/*
*    Error routine
*/
copy_error_close:
  fclose(source_stream);
  fclose(dest_stream);

copy_error:
  remove(dest_file);          // erase the unfinished file
  if (transfer_type == FILE_XFER_MOVE)
    cprintf("Error occurred while moving file - %s\r\n", source_file);
  else
    cprintf("Error occurred while copying to file - %s\r\n", dest_file);
  return -1;
  }  /* copy_single_file */

static int verify_file(char *master_file, char *verify_file)
  {
  FILE *mstream;
  FILE *vstream;
  char mtransfer_buffer[32768];
  char vtransfer_buffer[32768];
  int b;
  size_t mbyte_count, vbyte_count;
  struct stat mfile_st, vfile_st;


  /* Open files */
  mstream = fopen(master_file, "rb");
  if (mstream == NULL)
    goto verify_error;
  vstream = fopen(verify_file, "rb");
  if (vstream == NULL)
    {
    fclose(mstream);
    goto verify_error;
    }

  /* Verify file contents*/
  do
    {
    mbyte_count = fread(mtransfer_buffer, 1, 32768, mstream);
    vbyte_count = fread(vtransfer_buffer, 1, 32768, vstream);
    if (mbyte_count != vbyte_count)
      goto verify_error_close;
    if (mbyte_count > 0)
      {
      for (b = 0; b < mbyte_count; b++)
        {
        if (mtransfer_buffer[b] != vtransfer_buffer[b])
          goto verify_error_close;
        }
      }
    }
  while (mbyte_count > 0);

  /* verify date and time */
  if (fstat(fileno(mstream), &mfile_st) != 0)
    goto verify_error_close;
  if (fstat(fileno(vstream), &vfile_st) != 0)
    goto verify_error_close;
  if (mfile_st.st_atime != vfile_st.st_atime || mfile_st.st_mtime != vfile_st.st_mtime)
    goto verify_error_close;

  /* Close source and dest files */
  fclose(mstream);
  fclose(vstream);
  return 0;

/*
*    Error routine
*/
verify_error_close:
  fclose(mstream);
  fclose(vstream);

verify_error:
  cprintf("Verify failed - %s\r\n", verify_file);
  return -1;
  }

static void general_file_transfer(int transfer_type)
  {
  int xfer_count = 0;
  int ffrc;
  long ffhandle = 0;
  int traverse_subdirs = false;
  int copy_empty_subdirs = false;
  int xcopy_dont_ask_fd_question = false;
  int do_file_verify;
  int s, subdir_level = 0;
  finddata_t ff[MAX_SUBDIR_LEVEL];
  char dir_name[MAX_SUBDIR_LEVEL][MAXPATH];
  int visitation_mode[MAX_SUBDIR_LEVEL]; // 4 = findfirst source_filespec for files;
                                         // 3 = findnext source_filespec for files;
                                         // 2 = findfirst *.* for subdirs;
                                         // 1 = findnext *.* for subdirs;
                                         // 0 = done
  unsigned attrib;
  char drivespec[MAXDRIVE], dirspec[MAXDIR], filespec[MAXFILE], extspec[MAXEXT];
  char temp_path[MAXPATH];
  char source_path[MAXPATH] = "", source_filespec[MAXPATH];
  char dest_path[MAXPATH] = "", dest_filespec[MAXPATH];
  char full_source_filespec[MAXPATH];
  char full_dest_filespec[MAXPATH];
  char full_dest_dirspec[MAXPATH];

  if (transfer_type == FILE_XFER_MOVE)
    do_file_verify = true;
  else
    do_file_verify = false;

  while (*cmd_arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      if (*source_path == '\0')
        {
        strncpy(source_path, cmd_arg, MAXPATH);
        source_path[MAXPATH-1] = '\0';
        conv_unix_path_to_ms_dos(source_path);
        }
      else if (*dest_path == '\0')
        {
        strncpy(dest_path, cmd_arg, MAXPATH);
        dest_path[MAXPATH-1] = '\0';
        conv_unix_path_to_ms_dos(dest_path);
        }
      else
        {
        cprintf("Too many parameters - %s\r\n", cmd_args);
        reset_batfile_call_stack();
        return;
        }
      }
    else
      {
      if (stricmp(cmd_switch,"/v") == 0)
        {
        if (transfer_type == FILE_XFER_COPY ||
            transfer_type == FILE_XFER_XCOPY)
          do_file_verify = true;
        else
          goto InvalidSwitch;
        }
      else
        {
        if (transfer_type == FILE_XFER_XCOPY)
          {
          if (stricmp(cmd_switch,"/s") == 0)
            traverse_subdirs = true;
          else if (stricmp(cmd_switch,"/e") == 0)
            copy_empty_subdirs = true;
          else if (stricmp(cmd_switch,"/i") == 0)
            xcopy_dont_ask_fd_question = true;
          else
            goto InvalidSwitch;
          }
        else
          goto InvalidSwitch;
        }
      }
    advance_cmd_arg();
    }

  if (*source_path == '\0' ||
      (transfer_type == FILE_XFER_MOVE && *dest_path == '\0'))
    {
    cputs("Required parameter missing\r\n");
    reset_batfile_call_stack();
    return;
    }

  if (*dest_path == '\0')
    strcpy(dest_path, ".");

  // prepare source for fnsplit() -
  // attach a file specification if specified source doesn't have one
  if (is_drive_spec(source_path) || has_trailing_slash(source_path))
    strcat(source_path, "*.*");
  else
    {
    // see if source exists and is a directory; if so, attach a file spec
    if (file_access(source_path, D_OK) == 0)
      strcat(source_path, "\\*.*");
    }

  // parse source - create full source path and split into 2 components: path + file spec
  _fixpath(source_path, temp_path);
  fnsplit(temp_path, drivespec, dirspec, filespec, extspec);
  strcpy(source_path, drivespec);
  strcat(source_path, dirspec);
  conv_unix_path_to_ms_dos(source_path);
  strcpy(source_filespec, filespec);
  strcat(source_filespec, extspec);
  conv_unix_path_to_ms_dos(source_filespec);

  // prepare dest for fnsplit() -
  // attach a file specification if specified dest doesn't have one
  if (is_drive_spec(dest_path) || has_trailing_slash(dest_path))
    strcat(dest_path, "*.*");
  else
    {
    // see if dest exists and is a directory; if so, attach a file spec
    if (file_access(dest_path, D_OK) == 0)
      strcat(dest_path, "\\*.*");
    else  // else -- if dest does not exist or is not a directory...
      {
      if ((transfer_type == FILE_XFER_XCOPY && xcopy_dont_ask_fd_question) ||
          transfer_type == FILE_XFER_MOVE)
        {
        // if source has a wildcard and dest does not, then treat dest as a dir ...
        if (has_wildcard(source_filespec) && !has_wildcard(dest_path))
          strcat(dest_path, "\\*.*");     // dest is a directory; attach a file spec
        }
      else
        {
        if (transfer_type == FILE_XFER_XCOPY)  // if we are doing xcopy, ask if target is a dir or a file
          {
          fnsplit(dest_path, NULL, NULL, filespec, extspec);
          cprintf("Does %s%s specify a file name\r\n", filespec, extspec);
          cputs("or directory name on the target\r\n");
          cputs("(F = file, D = directory)?");
          if (get_choice("FD") == 'D')
            strcat(dest_path, "\\*.*");
          }
        }
      }
    }

  // parse dest - create full dest path and split into 2 components: path + file spec
  _fixpath(dest_path, temp_path);
  fnsplit(temp_path, drivespec, dirspec, filespec, extspec);
  strcpy(dest_path, drivespec);
  strcat(dest_path, dirspec);
  conv_unix_path_to_ms_dos(dest_path);
  strcpy(dest_filespec, filespec);
  strcat(dest_filespec, extspec);
  conv_unix_path_to_ms_dos(dest_filespec);

  // don't allow wildcard on the destination, except for *.*
  if ((has_wildcard(dest_filespec) &&
       strcmp(dest_filespec, "*.*") != 0) ||
      has_wildcard(dest_path))
    {
    cputs("Illegal wildcard on destination\r\n");
    reset_batfile_call_stack();
    return;
    }

  // Stuff for the move command only
  if (transfer_type == FILE_XFER_MOVE)
    {
    // if source and dest are both full directories in the same
    // tree and on the same drive, and the dest directory does not exist,
    // then just rename source directory to dest
    if (strcmp(source_filespec, "*.*") == 0 &&
        !is_drive_spec_with_slash(source_path) &&
        strcmp(dest_filespec, "*.*") == 0 &&
        !is_drive_spec_with_slash(dest_path))
      {
      char source_dirspec[MAXPATH];
      char dest_dirspec[MAXPATH];
      int dest_dir_exists;
      char *sbs, *dbs;  // backslash ptrs

      // check for both dirs to be at same tree and on the same drive
      strcpy(source_dirspec, source_path);
      strcpy(dest_dirspec, dest_path);
      *(strrchr(source_dirspec, '\\')) = '\0'; // get rid of trailing backslash
      *(strrchr(dest_dirspec, '\\')) = '\0'; // get rid of trailing backslash
      dest_dir_exists = (file_access(dest_dirspec, D_OK) == 0);
      sbs = strrchr(source_dirspec, '\\');
      *sbs = '\0'; // chop off source dir name, leaving source tree
      dbs = strrchr(dest_dirspec, '\\');
      *dbs = '\0'; // chop off dest dir name, leaving dest tree

      if (stricmp(source_dirspec, dest_dirspec) == 0) // if source tree == dest tree
        {
        if (!dest_dir_exists) // if dest dir does not exist..
          {
          *sbs = '\\'; // put the backslash back
          *dbs = '\\'; // put the backslash back
          if (rename(source_dirspec, dest_dirspec) == 0)
            {
            printf("%s renamed to %s\n", source_dirspec, dbs+1);
            return;
            }
          }
        }
      }
    }

  // visit each directory; perform transfer
  visitation_mode[0] = 4;
  dir_name[0][0] = '\0';
  while (subdir_level >= 0)
    {
    if (visitation_mode[subdir_level] == 4 || visitation_mode[subdir_level] == 2)
      {
      strcpy(full_source_filespec, source_path);
      for (s = 0; s <= subdir_level; s++)
        strcat(full_source_filespec, dir_name[s]);
      if (visitation_mode[subdir_level] == 4)
        {
        strcat(full_source_filespec, source_filespec);
        attrib = 0+FA_HIDDEN+FA_SYSTEM;
        }
      else
        {
        strcat(full_source_filespec, "*.*");
        attrib = 0+FA_DIREC+FA_HIDDEN+FA_SYSTEM;
        }
      ffrc = findfirst_f(full_source_filespec, &(ff[subdir_level]), attrib, &ffhandle);
      visitation_mode[subdir_level]--;
      }
    else
      ffrc = findnext_f(&(ff[subdir_level]), ffhandle);
    if (ffrc == 0)
      {
      conv_unix_path_to_ms_dos(FINDDATA_T_FILENAME(ff[subdir_level]));
      strcpy(full_source_filespec, source_path);
      strcpy(full_dest_filespec, dest_path);
      strcpy(full_dest_dirspec, dest_path);
      for (s = 0; s <= subdir_level; s++)
        {
        strcat(full_source_filespec, dir_name[s]);
        strcat(full_dest_filespec, dir_name[s]);
        strcat(full_dest_dirspec, dir_name[s]);
        }
      strcat(full_source_filespec, FINDDATA_T_FILENAME(ff[subdir_level]));
      if (strcmp(dest_filespec, "*.*") == 0)
        strcat(full_dest_filespec, FINDDATA_T_FILENAME(ff[subdir_level]));
      else
        strcat(full_dest_filespec, dest_filespec);

      if ((FINDDATA_T_ATTRIB(ff[subdir_level])&FA_DIREC) != 0)
        {
        if (visitation_mode[subdir_level] <= 2 &&
            traverse_subdirs &&
            strcmp(FINDDATA_T_FILENAME(ff[subdir_level]),".") != 0 &&
            strcmp(FINDDATA_T_FILENAME(ff[subdir_level]),"..") != 0)
          {
          subdir_level++;
          if (subdir_level >= MAX_SUBDIR_LEVEL)
            {
            cputs("Directory tree is too deep\r\n");
            reset_batfile_call_stack();
            goto ExitOperation;
            }
          if (copy_empty_subdirs)
            {
            if (ensure_dir_existence(full_dest_filespec) != 0)
              {
              reset_batfile_call_stack();
              goto ExitOperation;
              }
            }
          visitation_mode[subdir_level] = 4;
          strcpy(dir_name[subdir_level], FINDDATA_T_FILENAME(ff[subdir_level-1]));
          strcat(dir_name[subdir_level], "\\");
          }
        }
      else
        {
        if (visitation_mode[subdir_level] > 2)
          {
          if (transfer_type == FILE_XFER_XCOPY ||
              transfer_type == FILE_XFER_MOVE)
            {
            if (ensure_dir_existence(full_dest_dirspec) != 0)
              {
              reset_batfile_call_stack();
              goto ExitOperation;
              }
            }
          if (copy_single_file(full_source_filespec,
                               full_dest_filespec, transfer_type) != 0)
            {
            reset_batfile_call_stack();
            goto ExitOperation;
            }
          if (do_file_verify)
            {
            if (verify_file(full_source_filespec, full_dest_filespec) != 0)
              {
              reset_batfile_call_stack();
              goto ExitOperation;
              }
            }
          if (transfer_type == FILE_XFER_MOVE)
            {
            if (remove(full_source_filespec) != 0)
              {
              remove(full_dest_filespec);
              cprintf("Unable to move file - %s\r\n", full_source_filespec);
              reset_batfile_call_stack();
              goto ExitOperation;
              }
            }
          printf("%s %s to %s\n",
            FINDDATA_T_FILENAME(ff[subdir_level]),
            transfer_type == FILE_XFER_MOVE?"moved":"copied",
            strcmp(dest_filespec, "*.*")==0?full_dest_dirspec:full_dest_filespec);
          xfer_count++;
          }
        }
      }
    else
      {
      if (traverse_subdirs)
        visitation_mode[subdir_level]--;
      else
        visitation_mode[subdir_level] = 0;
      if (visitation_mode[subdir_level] <= 0)
        subdir_level--;
      }
    }
  if (xfer_count == 0)
    printf("File(s) not found - %s%s\n", source_path, source_filespec);
  else
    {
    if (transfer_type == FILE_XFER_MOVE)
      printf("%9d file(s) moved\n", xfer_count);
    else
      printf("%9d file(s) copied\n", xfer_count);
    }
ExitOperation:
  return;

InvalidSwitch:
  cprintf("Invalid switch - %s\r\n", cmd_switch);
  reset_batfile_call_stack();
  return;
  }

static int get_set_file_attribute(char *full_path_filespec, unsigned req_attrib, unsigned attrib_mask)
  {
  int a;
  unsigned actual_attrib;

  if (attrib_mask == 0)
    {
    if (getfileattr(full_path_filespec, &actual_attrib) != 0)
      {
      cprintf("Cannot read attribute - %s\r\n", full_path_filespec);
      return -1;
      }
    }
  else
    {
    if (getfileattr(full_path_filespec, &actual_attrib) != 0)
      actual_attrib = 0;
    actual_attrib &= (~attrib_mask);
    actual_attrib |= (req_attrib & attrib_mask);
    if (setfileattr(full_path_filespec, actual_attrib) != 0)
      goto CantSetAttr;
    printf("Attribute set to ");
    }

  for (a = 0; a < 4; a++)
    {
    if ((actual_attrib&attrib_values[a]) == 0)
      printf(" -%c",tolower(attrib_letters[a]));
    else
      printf(" +%c",toupper(attrib_letters[a]));
    }
  printf("  - %s\n", full_path_filespec);
  return 0;

CantSetAttr:
  cprintf("Cannot set attribute - %s\r\n", full_path_filespec);
  return -1;
  }

///////////////////////////////////////////////////////////////////////////////////

static void perform_attrib(const char *arg)
  {
  long ffhandle = 0;
  int ffrc;
  int file_count = 0;
  int traverse_subdirs = false;
  int s, subdir_level = 0;
  finddata_t ff[MAX_SUBDIR_LEVEL];
  char dir_name[MAX_SUBDIR_LEVEL][MAXPATH];
  int visitation_mode[MAX_SUBDIR_LEVEL]; // 4 = findfirst source_filespec for files;
                                         // 3 = findnext source_filespec for files;
                                         // 2 = findfirst *.* for subdirs;
                                         // 1 = findnext *.* for subdirs;
                                         // 0 = done
  char drivespec[MAXDRIVE], dirspec[MAXDIR], filename[MAXFILE], extspec[MAXEXT];
  char temp_path[MAXPATH];
  char path[MAXPATH] = "", filespec[MAXPATH];
  char full_path_filespec[MAXPATH];

  int a;
  unsigned req_attrib = 0, attrib_mask = 0;
  unsigned search_attrib;

  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      if (strlen(arg) == 2 && (arg[0] == '+' || arg[0] == '-'))
        {
        for (a = 0; a < 4; a++)
          {
          if (toupper(arg[1]) == toupper(attrib_letters[a]))
            {
            attrib_mask |= attrib_values[a];
            if (arg[0] == '+')
              req_attrib |= attrib_values[a];
            else
              req_attrib &= (~(attrib_values[a]));
            }
          }
        }
      else if (*path == '\0')
        {
        strncpy(path, arg, MAXPATH);
        path[MAXPATH-1] = '\0';
        conv_unix_path_to_ms_dos(path);
        }
      else
        {
        cprintf("Too many parameters - %s\r\n", cmd_args);
        reset_batfile_call_stack();
        return;
        }
      }
    else
      {
      if (stricmp(cmd_switch,"/s") == 0)
        traverse_subdirs = true;
      else
        {
        cprintf("Invalid switch - %s\r\n", cmd_switch);
        reset_batfile_call_stack();
        return;
        }
      }
    advance_cmd_arg();
    }

  if (*path == '\0')
    strcpy(path, "*.*");

  // prepare path for fnsplit() -
  // attach a file specification if specified path doesn't have one
  if (is_drive_spec(path) || has_trailing_slash(path))
    strcat(path, "*.*");
  else
    {
    // see if path exists and is a directory; if so, attach a file spec
    if (file_access(path, D_OK) == 0)
      strcat(path, "\\*.*");
    }

  // parse path - create full path and split into 2 components: path + file spec
  _fixpath(path, temp_path);
  fnsplit(temp_path, drivespec, dirspec, filename, extspec);
  strcpy(path, drivespec);
  strcat(path, dirspec);
  conv_unix_path_to_ms_dos(path);
  strcpy(filespec, filename);
  strcat(filespec, extspec);
  conv_unix_path_to_ms_dos(filespec);

  // visit each directory; perform attrib get/set
  visitation_mode[0] = 4;
  dir_name[0][0] = '\0';
  while (subdir_level >= 0)
    {
    if (visitation_mode[subdir_level] == 4 || visitation_mode[subdir_level] == 2)
      {
      strcpy(full_path_filespec, path);
      for (s = 0; s <= subdir_level; s++)
        strcat(full_path_filespec, dir_name[s]);
      if (visitation_mode[subdir_level] == 4)
        {
        strcat(full_path_filespec, filespec);
        search_attrib = FA_RDONLY+FA_ARCH+FA_SYSTEM+FA_HIDDEN;
        }
      else
        {
        strcat(full_path_filespec, "*.*");
        search_attrib = FA_DIREC+FA_RDONLY+FA_ARCH+FA_SYSTEM+FA_HIDDEN;
        }
      ffrc = findfirst_f(full_path_filespec, &(ff[subdir_level]), search_attrib, &ffhandle);
      visitation_mode[subdir_level]--;
      }
    else
      ffrc = findnext_f(&(ff[subdir_level]), ffhandle);
    if (ffrc == 0)
      {
      conv_unix_path_to_ms_dos(FINDDATA_T_FILENAME(ff[subdir_level]));
      strcpy(full_path_filespec, path);
      for (s = 0; s <= subdir_level; s++)
        strcat(full_path_filespec, dir_name[s]);
      strcat(full_path_filespec, FINDDATA_T_FILENAME(ff[subdir_level]));

      if ((FINDDATA_T_ATTRIB(ff[subdir_level])&FA_DIREC) != 0)
        {
        if (visitation_mode[subdir_level] <= 2 &&
            traverse_subdirs &&
            strcmp(FINDDATA_T_FILENAME(ff[subdir_level]),".") != 0 &&
            strcmp(FINDDATA_T_FILENAME(ff[subdir_level]),"..") != 0)
          {
          subdir_level++;
          if (subdir_level >= MAX_SUBDIR_LEVEL)
            {
            cputs("Directory tree is too deep\r\n");
            reset_batfile_call_stack();
            return;
            }
          visitation_mode[subdir_level] = 4;
          strcpy(dir_name[subdir_level], FINDDATA_T_FILENAME(ff[subdir_level-1]));
          strcat(dir_name[subdir_level], "\\");
          }
        }
      else
        {
        if (visitation_mode[subdir_level] > 2)
          {
          if (get_set_file_attribute(full_path_filespec, req_attrib, attrib_mask) != 0)
            {
            reset_batfile_call_stack();
            return;
            }
          file_count++;
          }
        }
      }
    else
      {
      if (traverse_subdirs)
        visitation_mode[subdir_level]--;
      else
        visitation_mode[subdir_level] = 0;
      if (visitation_mode[subdir_level] <= 0)
        subdir_level--;
      }
    }
  if (file_count == 0)
    printf("File(s) not found - %s%s\n", path, filespec);
  }

static void perform_call(const char *arg)
  {
  while (*cmd_switch)  // skip switches
    advance_cmd_arg();
  strcpy(cmd, arg);
  advance_cmd_arg();
  perform_external_cmd(true, cmd);
  }

static void perform_license(const char *arg)
  {
  const char *license =
    "comcom32 - COMMAND.COM-compatible command processor for DOS.\n\n"
    "Copyright (C) 1997, CENTROID CORPORATION, HOWARD, PA 16841\n"
    "Copyright (C) Allen S. Cheung (allencheung@fastmail.ca)\n"
    "Copyright (C) 2005, Hanzac Chen\n"
    "Copyright (C) 2019, C. Masloch <pushbx@38.de>\n"
    "Copyright (C) 2018-2020, stsp@users.sourceforge.net\n"
    "\n"
    "This program is free software: you can redistribute it and/or modify\n"
    "it under the terms of the GNU General Public License as published by\n"
    "the Free Software Foundation, either version 3 of the License, or\n"
    "(at your option) any later version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU General Public License for more details.\n";
  printf("%s\n", license);
  }

static void perform_loadhigh(const char *arg)
  {
  int orig_strat, orig_umblink;
  __dpmi_regs r = {};
  while (*cmd_switch)  // skip switches
    advance_cmd_arg();
  strcpy(cmd, arg);
  advance_cmd_arg();

  r.x.ax = 0x5800;
  __dpmi_int(0x21, &r);
  orig_strat = r.x.ax;
  r.x.ax = 0x5802;
  __dpmi_int(0x21, &r);
  orig_umblink = r.h.al;

  r.x.ax = 0x5801;
  r.x.bx = (orig_strat & 0xF) | 0x80;	/* set strat area = UMA-then-LMA */
  __dpmi_int(0x21, &r);
  r.x.ax = 0x5803;
  r.x.bx = 1;				/* set UMB link on */
  __dpmi_int(0x21, &r);

  perform_external_cmd(false, cmd);
	  /* Should we set this to true? Only affects batch files anyway,
	   * which shouldn't be loaded with LOADHIGH to begin with. */

  r.x.ax = 0x5801;
  r.x.bx = orig_strat;
  __dpmi_int(0x21, &r);
  r.x.ax = 0x5803;
  r.x.bx = orig_umblink;
  __dpmi_int(0x21, &r);
  }

static void perform_loadfix(const char *arg)
  {
  const int numblocks = 16;
  int orig_strat, orig_umblink, allocated, ii;
  unsigned short allocations[numblocks], allocation, to_64kib, max, size;
  __dpmi_regs r = {};
  int is_v = 0;
  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      break;
      }
    else
      {
      if (stricmp(cmd_switch,"/v")==0)
        {
        is_v = 1;
        }
      else
        {
        cprintf("Invalid switch - %s\r\n", cmd_switch);
        reset_batfile_call_stack();
        return;
        }
      }
    advance_cmd_arg();
    }
  strcpy(cmd, arg);
  advance_cmd_arg();

  r.x.ax = 0x5800;
  __dpmi_int(0x21, &r);
  orig_strat = r.x.ax;
  r.x.ax = 0x5802;
  __dpmi_int(0x21, &r);
  orig_umblink = r.h.al;

  r.x.ax = 0x5801;
  r.x.bx = 0;				/* set strat LMA-then-UMA first-fit */
  __dpmi_int(0x21, &r);
  r.x.ax = 0x5803;
  r.x.bx = 0;				/* set UMB link off */
  __dpmi_int(0x21, &r);

  ii = 0;
  do
    {
    r.h.ah = 0x48;
    r.x.bx = 1;
    __dpmi_int(0x21, &r);		/* allocate one-paragraph block */
    if ((r.x.flags & 1) == 0)		/* if NC */
      {
      allocated = 1;
      allocation = r.x.ax;
      if (is_v)
        {
        printf("LOADFIX: allocated block at %04Xh\n", allocation);
        }
      if (allocation >= 0x1000) 	/* does it start above 64 KiB ? */
        {
        r.h.ah = 0x49;
        r.x.es = allocation;
        __dpmi_int(0x21, &r);		/* free */
        if (is_v)
          {
          printf("LOADFIX: too high, freeing block at %04Xh\n", allocation);
          }
        break;				/* and done */
        }
      if (ii >= numblocks)
        {
        printf("LOADFIX: too many blocks allocated!\n");
        break;
        }
      allocations[ii] = allocation;
      ++ii;
      r.h.ah = 0x4A;
      r.x.bx = -1;
      r.x.es = allocation;
      __dpmi_int(0x21, &r);		/* resize and get maximum block size */
		/* Note that this expands the block to the maximum
		 * available size. */
      max = r.x.bx;
      to_64kib = 0x1000 - allocation;	/* note: does not underflow */
      size = to_64kib < max ? to_64kib : max;
      r.x.bx = size;
      r.h.ah = 0x4A;
      __dpmi_int(0x21, &r);		/* resize */
		/* If to_64kib is the lower value, this shortens the block
		 * to that size. Else it does nothing. */
      if (is_v)
        {
        printf("LOADFIX: resizing block at %04Xh to %04Xh paragraphs (%u bytes)\n",
		allocation, (int)size, (int)size * 16);
        }
      }
    else
      {
      if (is_v)
        {
        printf("LOADFIX: could not allocate another block\n");
        }
      allocated = 0;
      }
    }
  while (allocated);

  r.x.ax = 0x5801;
  r.x.bx = orig_strat;
  __dpmi_int(0x21, &r);
  r.x.ax = 0x5803;
  r.x.bx = orig_umblink;
  __dpmi_int(0x21, &r);

  perform_external_cmd(false, cmd);
	  /* Should we set this to true? Only affects batch files anyway,
	   * which shouldn't be loaded with LOADFIX to begin with. */

  while (ii != 0)
    {
    --ii;
    r.h.ah = 0x49;
    r.x.es = allocations[ii];
    __dpmi_int(0x21, &r);		/* free */
    if (is_v)
      {
      printf("LOADFIX: afterwards freeing block at %04Xh\n", allocations[ii]);
      }
    }
  }

static void perform_cd(const char *arg)
  {
  while (*cmd_switch)  // skip switches
    advance_cmd_arg();
  if (*arg)
    {
    if (chdir(arg) != 0)
      {
      cprintf("Directory does not exist - %s\r\n",arg);
      reset_batfile_call_stack();
      return;
      }
    }
  else
    {
    char cur_drive_and_path[MAXPATH];
    getcwd(cur_drive_and_path, MAXPATH);
    conv_unix_path_to_ms_dos(cur_drive_and_path);
    puts(cur_drive_and_path);
    }
  }

static void perform_change_drive(void)
  {
  char cur_drive_and_path[MAXPATH];
  unsigned int drive_set, cur_drive = 0, old_drive, dummy;
  drive_set = toupper(cmd[0])-'A'+1;
  getdrive(&old_drive);
  setdrive(drive_set, &dummy);
  getdrive(&cur_drive);
  if (cur_drive != drive_set)
    {
    cprintf("Invalid drive specification - %s\r\n", cmd);
    reset_batfile_call_stack();
    return;
    }
  if (!getcwd(cur_drive_and_path, MAXPATH))
    {
    cprintf("Drive not ready - %s\r\n", cmd);
    reset_batfile_call_stack();
    setdrive(old_drive, &dummy);
    return;
    }
  }

static void perform_choice(const char *arg)
  {
  const char *choices = "YN";  // Y,N are the default choices
  const char *text = "";
  int supress_prompt = false;
  int choice;

  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      text = cmd_args;
      break;
      }
    else
      {
      if (strnicmp(cmd_switch,"/c:", 3) == 0 && strlen(cmd_switch) > 3)
        choices = cmd_switch+3;
      else if (stricmp(cmd_switch,"/n") == 0)
        supress_prompt = true;
      else
        {
        cprintf("Invalid switch - %s\r\n", cmd_switch);
        reset_batfile_call_stack();
        return;
        }
      }
    advance_cmd_arg();
    }

  cputs(text);
  if (!supress_prompt)
    {
    int first = true;
    const char *c;

    putch('[');
    c = choices;
    while (*c != '\0')
      {
      if (first)
        first = false;
      else
        putch(',');
      putch(toupper(*c));
      c++;
      }
    cputs("]?");
    }
  choice = get_choice(choices);
  error_level = strchr(choices, choice) - choices + 1;
  return;
  }

static void perform_copy(const char *arg)
  {
  general_file_transfer(FILE_XFER_COPY);
  }

static void perform_xcopy(const char *arg)
  {
  general_file_transfer(FILE_XFER_XCOPY);
  }

static void perform_date(const char *arg)
  {
  time_t t = time(NULL);
  struct tm *loctime = localtime (&t);
  const char *day_of_week[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  printf("Current date is %s %02d-%02d-%04d\n", day_of_week[loctime->tm_wday],
                             loctime->tm_mon+1, loctime->tm_mday, loctime->tm_year+1900);
  }

static void perform_delete(const char *arg)
  {
  finddata_t ff;
  char filespec[MAXPATH] = "";
  char full_filespec[MAXPATH] = "";
  char drive[MAXDRIVE], dir[MAXDIR];

  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      if (*filespec == '\0')
        {
        strncpy(filespec, arg, MAXPATH);
        filespec[MAXPATH-1] = '\0';
        }
      else
        {
        cprintf("Too many parameters - %s\r\n", cmd_args);
        reset_batfile_call_stack();
        return;
        }
      }
    advance_cmd_arg();
    }
  if (*filespec == '\0')
    {
    cprintf("filespec not specified\r\n");
    reset_batfile_call_stack();
    return;
    }

  if (findfirst_f(filespec, &ff, 0, NULL) != 0)
    {
    printf("File(s) not found - %s\n", filespec);  // informational msg; not an error
    return;
    }

  _fixpath(filespec, full_filespec);
  fnsplit(full_filespec, drive, dir, NULL, NULL);
  conv_unix_path_to_ms_dos(drive);
  conv_unix_path_to_ms_dos(dir);

  while (findfirst_f(full_filespec, &ff, 0, NULL) == 0)
    {
    char individual_filespec[MAXPATH];

    conv_unix_path_to_ms_dos(FINDDATA_T_FILENAME(ff));
    strcpy(individual_filespec, drive);
    strcat(individual_filespec, dir);
    strcat(individual_filespec, FINDDATA_T_FILENAME(ff));

    if (remove(individual_filespec) == 0)
      printf("%s erased\n", individual_filespec);
    else
      {
      cprintf("Access denied - %s\r\n", individual_filespec);
      reset_batfile_call_stack();
      return;
      }
    }
  }

static void perform_deltree(const char *arg)
  {
  long ffhandle = 0;
  int ffrc;
  int file_count = 0, dir_count = 0;
  int s, subdir_level = 0;
  int confirm_before_delete = true;
  finddata_t ff[MAX_SUBDIR_LEVEL];
  char dir_name[MAX_SUBDIR_LEVEL][MAXPATH];
  int remove_level1_dir;
  int visitation_mode[MAX_SUBDIR_LEVEL]; // 4 = findfirst source_filespec for files;
                                         // 3 = findnext source_filespec for files;
                                         // 2 = findfirst *.* for subdirs;
                                         // 1 = findnext *.* for subdirs;
                                         // 0 = done
  char drivespec[MAXDRIVE], dirspec[MAXDIR], filename[MAXFILE], extspec[MAXEXT];
  char temp_path[MAXPATH];
  char path[MAXPATH] = "", filespec[MAXPATH];
  char full_path_filespec[MAXPATH];
  int choice;
  unsigned search_attrib;

  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      if (*path == '\0')
        {
        strncpy(path, arg, MAXPATH);
        path[MAXPATH-1] = '\0';
        conv_unix_path_to_ms_dos(path);
        }
      else
        {
        cprintf("Too many parameters - %s\r\n", cmd_args);
        reset_batfile_call_stack();
        return;
        }
      }
    else
      {
      if (stricmp(cmd_switch,"/Y") == 0)
        confirm_before_delete = false;
      else
        {
        cprintf("Invalid switch - %s\r\n", cmd_switch);
        reset_batfile_call_stack();
        return;
        }
      }
    advance_cmd_arg();
    }
  if (*path == '\0')
    {
    cprintf("filespec not specified\r\n");
    reset_batfile_call_stack();
    return;
    }

  // prepare path for fnsplit() -
  // attach a file specification if specified path doesn't have one
  if (is_drive_spec(path) || has_trailing_slash(path))
    strcat(path, "*.*");

  // parse path - create full path and split into 2 components: path + file spec
  _fixpath(path, temp_path);
  fnsplit(temp_path, drivespec, dirspec, filename, extspec);
  strcpy(path, drivespec);
  strcat(path, dirspec);
  conv_unix_path_to_ms_dos(path);
  strcpy(filespec, filename);
  strcat(filespec, extspec);
  conv_unix_path_to_ms_dos(filespec);

  // visit each directory; delete files and subdirs
  visitation_mode[0] = 4;
  dir_name[0][0] = '\0';
  remove_level1_dir = false;
  while (subdir_level >= 0)
    {
    if (visitation_mode[subdir_level] == 4 || visitation_mode[subdir_level] == 2)
      {
      strcpy(full_path_filespec, path);
      for (s = 0; s <= subdir_level; s++)
        strcat(full_path_filespec, dir_name[s]);
      if (subdir_level == 0)
        strcat(full_path_filespec, filespec);
      else
        strcat(full_path_filespec, "*.*");
      if (visitation_mode[subdir_level] == 4)
        search_attrib = FA_RDONLY+FA_ARCH+FA_SYSTEM+FA_HIDDEN;
      else
        search_attrib = FA_DIREC+FA_RDONLY+FA_ARCH+FA_SYSTEM+FA_HIDDEN;
      ffrc = findfirst_f(full_path_filespec, &(ff[subdir_level]), search_attrib, &ffhandle);
      visitation_mode[subdir_level]--;
      }
    else
      ffrc = findnext_f(&(ff[subdir_level]), ffhandle);
    if (ffrc == 0)
      {
      conv_unix_path_to_ms_dos(FINDDATA_T_FILENAME(ff[subdir_level]));
      strcpy(full_path_filespec, path);
      for (s = 0; s <= subdir_level; s++)
        strcat(full_path_filespec, dir_name[s]);
      strcat(full_path_filespec, FINDDATA_T_FILENAME(ff[subdir_level]));

      if ((FINDDATA_T_ATTRIB(ff[subdir_level])&FA_DIREC) != 0)
        {
        if (visitation_mode[subdir_level] <= 2 &&
            strcmp(FINDDATA_T_FILENAME(ff[subdir_level]),".") != 0 &&
            strcmp(FINDDATA_T_FILENAME(ff[subdir_level]),"..") != 0)
          {
          if (subdir_level == 0)
            {
            if (confirm_before_delete)
              {
              cprintf("Delete directory %s and all its subdirectories? [Y/N] ", full_path_filespec);
              remove_level1_dir = (get_choice("YN") == 'Y');
              }
            else
              remove_level1_dir = true;
            }
          if (subdir_level > 0 || remove_level1_dir)
            {
            subdir_level++;
            if (subdir_level >= MAX_SUBDIR_LEVEL)
              {
              cputs("Directory tree is too deep\r\n");
              reset_batfile_call_stack();
              return;
              }
            visitation_mode[subdir_level] = 4;
            strcpy(dir_name[subdir_level], FINDDATA_T_FILENAME(ff[subdir_level-1]));
            strcat(dir_name[subdir_level], "\\");
            }
          }
        }
      else
        {
        if (visitation_mode[subdir_level] > 2)
          {
          if (confirm_before_delete && subdir_level == 0)
            {
            cprintf("Delete file %s ? [Y/N] ", full_path_filespec);
            choice = get_choice("YN");
            }
          else
            choice = 'Y';
          if (choice == 'Y')
            {
            if (remove(full_path_filespec) != 0)
              {
              cprintf("Unable to delete file - %s\r\n", full_path_filespec);
              reset_batfile_call_stack();
              return;
              }
            if (subdir_level == 0)
              printf("%s deleted\n", full_path_filespec);
            file_count++;
            }
          }
        }
      }
    else
      {
      visitation_mode[subdir_level]--;
      if (visitation_mode[subdir_level] <= 0)
        {
        if (subdir_level > 0)
          {
          strcpy(full_path_filespec, path);
          for (s = 0; s <= subdir_level; s++)
            strcat(full_path_filespec, dir_name[s]);
          *(strrchr(full_path_filespec,'\\')) = '\0';
          if (subdir_level > 1 || remove_level1_dir)
            {
            if (rmdir(full_path_filespec) != 0)
              {
              cprintf("Unable to remove directory - %s\\\r\n", full_path_filespec);
              reset_batfile_call_stack();
              return;
              }
            if (subdir_level == 1)
              printf("%s removed\n", full_path_filespec);
            dir_count++;
            }
          }
        subdir_level--;
        if (subdir_level >= 0)
          visitation_mode[subdir_level] = 4;  // restart from findfirst
        }
      }
    }
  printf("%9d file(s) deleted, ", file_count);
  if (dir_count == 1)
    printf("%9d directory removed\n", dir_count);
  else
    printf("%9d (sub)directories removed\n", dir_count);
  }

static void perform_dir(const char *arg)
  {
  long ffhandle;
  int ffrc;
  int wide_column_countdown = -1;
  unsigned long long avail; //was double avail; --Salvo
  finddata_t ff;
  diskfree_t df;
  unsigned int attrib = FA_DIREC+FA_RDONLY+FA_ARCH+FA_SYSTEM+FA_HIDDEN, first;
  unsigned long filecount = 0, dircount = 0, bytecount = 0;
  char dirspec[MAXPATH];
  char volspec[7] = "X:\\*.*";
  char full_filespec[MAXPATH];
  char filespec[MAXPATH] = "";

  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      if (*filespec == '\0')
        {
        strncpy(filespec, arg, MAXPATH);
        filespec[MAXPATH-1] = '\0';
        conv_unix_path_to_ms_dos(filespec);
        }
      else
        {
        cprintf("Too many parameters - %s\r\n", cmd_args);
        reset_batfile_call_stack();
        return;
        }
      }
    else
      {
      if (stricmp(cmd_switch,"/w")==0)
        wide_column_countdown = 5;
      }
    advance_cmd_arg();
    }

  if (!has_trailing_slash(filespec) && !has_wildcard(filespec) &&
      findfirst_f(filespec, &ff, FA_DIREC, NULL) == 0)
    strcat(filespec, "\\");

  if (*filespec == '\0' || is_drive_spec(filespec) ||
      has_trailing_slash(filespec))
    strcat(filespec, "*.*");
  _fixpath(filespec, full_filespec);
  conv_unix_path_to_ms_dos(full_filespec);

  volspec[0] = full_filespec[0];
  if (findfirst_f(volspec, &ff, FA_LABEL, NULL) == 0)
    {
    char *p = strchr(FINDDATA_T_FILENAME(ff), '.');
    if (p)
      memmove(p, p + 1, strlen(p + 1) + 1);
    printf(" Volume in drive %c is %s\n", volspec[0], FINDDATA_T_FILENAME(ff));
    }
  else
    puts(" Volume has no label");

  fnsplit (full_filespec, NULL, dirspec, NULL, NULL);
  printf(" Directory of %c:%s\n\n", full_filespec[0], dirspec);

  first = true;
  for (;;)
    {
    if (first)
      {
      if ((ffrc = findfirst_f(full_filespec, &ff, attrib, &ffhandle)) != 0)
        {
        puts("File not found");  // informational message -- not an error
        return;
        }
      first = false;
      }
    else
      {
      if ((ffrc = findnext_f(&ff, ffhandle)) != 0)
        break;
      }
    conv_unix_path_to_ms_dos(FINDDATA_T_FILENAME(ff));
    if (wide_column_countdown < 0)
      {
      printf("%04d-%02d-%02d ", FINDDATA_T_WDATE_YEAR(ff), FINDDATA_T_WDATE_MON(ff), FINDDATA_T_WDATE_DAY(ff));
      printf("%02d:%02d ", FINDDATA_T_WTIME_HOUR(ff), FINDDATA_T_WTIME_MIN(ff));
      if ((FINDDATA_T_ATTRIB(ff)&FA_DIREC) == 0)
        printf("%13lu", FINDDATA_T_SIZE(ff));
      else
        printf("<DIR>%8s", "");
      printf(" %s\n", FINDDATA_T_FILENAME(ff));
      }
    else
      {
      if ((FINDDATA_T_ATTRIB(ff)&FA_DIREC) == 0)
        printf("%-14s", FINDDATA_T_FILENAME(ff));
      else
        {
        int len = strlen(FINDDATA_T_FILENAME(ff)) + 2;
        printf("[%s]", FINDDATA_T_FILENAME(ff));
        while (len < 14)
          {
          printf(" ");
          len++;
          }
        }
      wide_column_countdown--;
      if (wide_column_countdown == 0)
        {
        puts("");
        wide_column_countdown = 5;
        }
      else
        printf("  ");
      }

    if ((FINDDATA_T_ATTRIB(ff)&FA_DIREC) == 0)
      {
      filecount++;
      bytecount += FINDDATA_T_SIZE(ff);
      }
    else
      dircount++;
    }
  if (wide_column_countdown >= 0 && wide_column_countdown < 5)
    puts("");
  printf("%10lu file(s) %14lu bytes\n", filecount, bytecount);
  printf("%10lu dir(s) ", dircount);

  getdfree(full_filespec[0]-'a'+1, &df);
  avail = DISKFREE_T_AVAIL(df)*DISKFREE_T_BSEC(df)*DISKFREE_T_SCLUS(df);

  if (avail < 1048576)
    printf("%15lli byte(s) free\n", avail);
  else if (avail < 1073741824)
    printf("%15lli KB free\n", avail / 1024);
  else if (avail < 2147483648ULL)
    printf("%15lli MB free\n", avail / 1024 / 1024);
  else
    printf("%15.1f GB free\n", avail / 1024.0 / 1024.0 / 1024.0);
  }

static void perform_echo(const char *arg)
  {
  if (stricmp(arg, "off") == 0)
    echo_on[stack_level] = false;
  else if (stricmp(arg, "on") == 0)
    echo_on[stack_level] = true;
  else if (arg[0] == '\0')
    {
    if (echo_on[stack_level])
      puts("ECHO is on");
    else
      puts("ECHO is off");
    }
  else
    puts(cmd_args);
  }

static void perform_echo_dot(const char *arg)
  {
  if (arg[0] == '\0')
    puts("");
  else
    puts(cmd_args);
  }

static void perform_exit(const char *arg)
  {
  int ba;
  int is_bat = bat_file_path[stack_level][0];
  bat_file_path[stack_level][0] = '\0';
  for (ba = 0; ba < MAX_BAT_ARGS; ba++)
    bat_arg[stack_level][ba][0] = '\0';
  bat_file_line_number[stack_level] = 0;
  echo_on[stack_level] = true;
  if (stack_level > 0)
    stack_level--;
  else
    {
    if (!shell_permanent || (getenv("SHELL_ALLOW_EXIT") && !is_bat))
      {
      exiting++;
      if (arg)
        {
        if (arg[0])
          error_level = atoi(arg);
        else
          error_level = 0;
        }
      }
    }
  }

struct for_iter {
  char *token;
  int glob_idx;
  int glob_state;
  glob_t gl;
  const char *end;
  char *sptr;
};

static void advance_iter(struct for_iter *iter)
  {
  char *tok = strtok_r(NULL, " )", &iter->sptr);
  iter->token = ((tok && tok < iter->end) ? tok : NULL);
  }

static const char *extract_token(struct for_iter *iter)
  {
  const char *tok;

  if (iter->glob_state == 2)
    {
    globfree(&iter->gl);
    iter->glob_state = 0;
    }

again:
  if (!iter->token)
    return NULL; // no more tokens
  if (iter->glob_state)
    {
    tok = iter->gl.gl_pathv[iter->glob_idx++];
    if (iter->glob_idx >= iter->gl.gl_pathc)
      {
      iter->glob_state = 2;
      advance_iter(iter);
      }
    return tok;
    }
  if (!has_wildcard(iter->token))
    {
    tok = iter->token;
    advance_iter(iter);
    return tok;
    }
  strupr(iter->token);
  if (glob(iter->token, 0, NULL, &iter->gl))
    {
    advance_iter(iter);
    goto again;
    }
  tok = iter->gl.gl_pathv[0];
  if (iter->gl.gl_pathc > 1)
    {
    iter->glob_state = 1;
    iter->glob_idx = 1;
    }
  else
    {
    iter->glob_state = 2;
    advance_iter(iter);
    }
  return tok;
  }

static void perform_for(const char *arg)
  {
  const char *tok;
  char cmd_args2[MAX_CMD_BUFLEN];
  struct for_iter iter = {};
  char *p, *p1, *d0, *d1, *d, *c;
  const char *v;

  strcpy(cmd_args2, cmd_args);
  p = strchr(cmd_args2, '(');
  p1 = strchr(cmd_args2, ')');
  d0 = strstr(cmd_args2, " DO ");
  d1 = strstr(cmd_args2, " do ");
  d = d0 ?: d1;
  if (!p || !p1 || p1 < p || !d)
    {
    cprintf("Syntax error\r\n");
    reset_batfile_call_stack();
    return;
    }
  v = arg;
  if (*v == '%')
    v++;
  for_var = *v;
  p++;
  iter.token = strtok_r(p, " )", &iter.sptr);
  iter.end = p1;
  c = d + 4;
  while (*c == ' ')
    c++;
  while ((tok = extract_token(&iter)))
    {
    strlcpy(cmd_line, c, sizeof(cmd_line));
    for_val = tok;
    parse_cmd_line();
    exec_cmd();
    }
  for_var = '\0';
  }

/* this function replaces RM env (pointed to with env_sel) with
 * PM env (environ[]), leaving tail intact */
static void put_env(unsigned short env_sel)
{
  int env_count;
  int env_offs = 0;
  char *env;
  char *tail;
  unsigned env_size = __dpmi_get_segment_limit(env_sel) + 1;
  int tail_sz = 3;

  env = alloca(env_size + tail_sz);
  /* back up full env, just for getting its tail */
  movedata(env_sel, 0, _my_ds(), (unsigned)env, env_size);
  memset(&env[env_size], 0, tail_sz);
  tail = memchr(env, 1, env_size);
  if (tail && tail[1] == '\0') {
    tail_sz += strlen(tail + 2) + 1;
    tail--;
  } else {
    tail = memmem(env, env_size, "\x0\x0", 2);
    if (!tail) {
      printf("ENV block corrupted\n");
      return;
    }
    tail++;
    if (tail - env + tail_sz > env_size || memcmp(tail, "\x0\x0\x0", 3) != 0)
      tail_sz = 1;  /* DOS-2.0 terminator */
  }
  /* now put entire environ[] down, overwriting prev content */
  for (env_count = 0; environ[env_count]; env_count++) {
    int l = strlen(environ[env_count]) + 1;
    if (env_offs + l >= env_size - tail_sz) {
      printf("ENV buffer overflow (size %u, need %u, tail %i)\n",
          env_size, env_offs + l, tail_sz);
      break;
    }
    movedata(_my_ds(), (unsigned)environ[env_count], env_sel, env_offs, l);
    env_offs += l;
  }
  /* and preserve tail */
  if (env_offs + tail_sz <= env_size)
    movedata(_my_ds(), (unsigned)tail, env_sel, env_offs, tail_sz);
}

#if !SYNC_ENV
static void set_env(const char *variable, const char *value,
    unsigned short env_sel, unsigned env_size)
{
  char *env;
  char *tail;
  char *cp;
  char *env2;
  int l;
  int len;
  int tail_sz = 3;

  /* allocate tmp buffer for env and copy them there */
  env = alloca(env_size + tail_sz);
  movedata(env_sel, 0, _my_ds(), (unsigned)env, env_size);
  memset(&env[env_size], 0, tail_sz);
  cp = env2 = env;
  l = strlen(variable);
  len = l + strlen(value) + 2;
  /*
     Delete any existing variable with the name (var).
  */
  while (*env2 && (env2 - env) < env_size) {
    if ((strncmp(variable, env2, l) == 0) && (env2[l] == '=')) {
      cp = env2 + strlen(env2) + 1;
      memmove(env2, cp, env_size - (cp - env));
    } else {
      env2 += strlen(env2) + 1;
    }
  }

  tail = env2;
  cp = tail + 1;
  if (cp[0] == '\1' && cp[1] == '\0')
    tail_sz += strlen(cp + 2) + 1;

  /*
     If the variable fits, shovel it in at the end of the envrionment.
  */
  if (value && value[0] && (env_size - (env2 - env) - tail_sz >= len)) {
    memmove(env2 + len, env2, tail_sz);
    strcpy(env2, variable);
    strcat(env2, "=");
    strcat(env2, value);
  }

  /* now put it back */
  movedata(_my_ds(), (unsigned)env, env_sel, 0, env_size);
}

static void sync_env(void)
{
  unsigned short sel;
  unsigned short psp = _stubinfo->psp_selector;
  movedata(psp, 0x2c, _my_ds(), (unsigned)&sel, 2);
  put_env(sel);
}
#endif

static void perform_external_cmd(int call, char *ext_cmd)
  {
  finddata_t ff;
  long ffhandle;
  char cmd_name[MAX_CMD_BUFLEN];
  char *pathvar, pathlist[200];
  char full_cmd[MAXPATH+MAX_CMD_BUFLEN] = "";
  char temp_cmd[MAXPATH+MAX_CMD_BUFLEN];
  int rc;
  int exec_type, e, ba;
  const char *exec_ext[3] = {".COM",".EXE",".BAT"};
  char *s;

  // No wildcards allowed -- reject them
  if (has_wildcard(ext_cmd))
    goto BadCommand;

  // Assemble a path list, and also extract the command name without the path.
  // For commands that already specify a path, the path list will consist of
  // only that path.
  s = strrchr(ext_cmd, '\\');
  if (s != NULL)
    {
    s++;
    strncpy(pathlist, ext_cmd, s-ext_cmd);
    pathlist[s-ext_cmd] = '\0';
    }
  else
    {
    s = strchr(ext_cmd, ':');
    if (s != NULL)
      {
      s++;
      strncpy(pathlist, ext_cmd, s-ext_cmd);
      pathlist[s-ext_cmd] = '\0';
      }
    else
      {
      strcpy(pathlist, ".\\;");
      s = ext_cmd;
      pathvar = getenv("PATH");
      if(pathvar != NULL)
        {
        strncat(pathlist, pathvar, 200 - strlen(pathlist) - 1);
        pathlist[sizeof(pathlist)-1] = '\0';
        }
      }
    }
  strcpy(cmd_name, s);

  // Search for the command
  exec_type = -1;
  pathvar = strtok(pathlist, "; ");
  while (pathvar != NULL)
    {
    // start to build full command name (sort of, because it still could be missing .exe, .com, or .bat)
    strcpy(full_cmd, pathvar);
    s = strchr(full_cmd, '\0');
    if (strlen(full_cmd) > 0)
      {
      if(*(s-1)!=':'&&*(s-1)!='\\')
        {
        *s = '\\';
        s++;
        *s = '\0';
        }
      }
    if (stricmp(full_cmd,".\\") == 0)
      full_cmd[0] = '\0';
    strcat(full_cmd, cmd_name);
    _fixpath(full_cmd, temp_cmd);
    strcpy(full_cmd, temp_cmd);
    conv_unix_path_to_ms_dos(full_cmd);

    // check validity for each executable type
    for (e = 0; e < 3; e++)
      {
      s = strchr(cmd_name, '.');
      if (s == NULL)  // no file type mentioned
        {
        s = strchr(full_cmd, '\0');  // save position of the nul terminator
        strcat(full_cmd, exec_ext[e]);
        if (findfirst_f(full_cmd, &ff, 0, &ffhandle) == 0)
          {
          exec_type = e;
          break;
          }
        *s = '\0'; // restore nul terminator
        }
      else
        {
        if (stricmp(s, exec_ext[e]) == 0)
          {
          if (findfirst_f(full_cmd, &ff, 0, &ffhandle) == 0)
            {
            exec_type = e;
            break;
            }
          }
        }
      }

    if (exec_type < 0)  // if exec file not found yet...
      pathvar = strtok(NULL, "; ");  // try next path in path list
    else
      pathvar = NULL;   // command found...
    }

  if (exec_type < 0)
    goto BadCommand;

  strupr(full_cmd);

  if (exec_type == 2)  // if command is a batch file
    {
    if (call)
      {
      stack_level++;
      if (stack_level >= MAX_STACK_LEVEL)
        goto StackOverflow;
      }
    else
      bat_file_line_number[stack_level] = 0;
    strcpy(bat_file_path[stack_level], full_cmd);
    ba = 0;
    /* keep last entry empty to simplify shifting */
    while (ba < MAX_BAT_ARGS - 1 && *cmd_arg != '\0')
      {
      strcpy(bat_arg[stack_level][ba], cmd_arg);
      advance_cmd_arg();
      ba++;
      }
    }
  else
    {
    unsigned short psp = _stubinfo->psp_selector;
    unsigned short env_sel;
    unsigned short env_seg;
    unsigned long env_addr;
    char el[16];
    int err;
    int alen;
    int env_chg = 0;

    movedata(psp, 0x2c, _my_ds(), (unsigned)&env_sel, 2);
    err = __dpmi_get_segment_base_address(env_sel, &env_addr);
    if (!err && !(env_addr & 0xf) && env_addr < 0x110000) {
      env_seg = env_addr >> 4;
      movedata(_my_ds(), (unsigned)&env_seg, psp, 0x2c, 2);
#if SYNC_ENV
      /* the below is disabled because it seems we don't need
       * to update our copy of env. djgpp creates the env segment
       * for the child process from the prot-mode environment anyway.
       * Disabling allows to pass much fewer memory to /E.
       * But we still need the /E because some programs (msetenv)
       * may set the env strings on their parent (shell) to make
       * them permanent. */
      put_env(env_sel);
#else
      set_env("PATH", getenv("PATH"), env_sel,
          __dpmi_get_segment_limit(env_sel) + 1);
#endif
      env_chg = 1;
    }
    _control87(0x033f, 0xffff);
#ifdef __DJGPP__
    __djgpp_exception_toggle();
#endif
    /* prepend command tail with space */
    alen = strlen(cmd_args) + 1;
    if (alen >= MAX_CMD_BUFLEN)
      {
      alen = MAX_CMD_BUFLEN - 1;
      cmd_args[alen - 1] = '\0';
      }
    memmove(cmd_args + 1, cmd_args, alen);
    cmd_args[0] = ' ';
    rc = _dos_exec(full_cmd, cmd_args, environ, 0);
    if (rc == -1)
      cprintf("Error: unable to execute %s\r\n", full_cmd);
    else
      error_level = rc & 0xff;
#ifdef __DJGPP__
    __djgpp_exception_toggle();
#endif
    _control87(0x033f, 0xffff);
    _clear87();
    _fpreset();
    gppconio_init();  /* video mode could change */
    if (env_chg) {
      char *cp;
      char *dos_environ;
      unsigned long env_size;

      env_size = __dpmi_get_segment_limit(env_sel) + 1;
      dos_environ = alloca(env_size);
      movedata(_my_ds(), (unsigned)&env_sel, psp, 0x2c, 2);
      movedata(env_sel, 0, _my_ds(), (unsigned)dos_environ, env_size);
      dos_environ[env_size] = 0;
      cp = dos_environ;
      do {
        if (*cp) {
          char *env = strdup(cp);
          putenv(env);
          cp += strlen(env);
        }
        cp++; /* skip to next character */
      } while (*cp); /* repeat until two NULs */
    }
    sprintf(el, "%d", error_level);
    setenv("ERRORLEVEL", el, 1);
    }
  return;

BadCommand:
  cprintf("Bad command or file name - %s\r\n", ext_cmd);
  //reset_batfile_call_stack();  -- even if error becuase the command is not found, don't clean up
  return;

StackOverflow:
  cputs("Call stack overflow\r\n");
  reset_batfile_call_stack();
  return;
  }

static void perform_goto(const char *arg)
  {
  if (bat_file_path[stack_level][0] != '\0')
    {
    strcpy(goto_label, arg);
    bat_file_line_number[stack_level] = MAXINT;
    }
  else
    cputs("Goto not valid in immediate mode.\r\n");
  }

static void perform_help(const char *arg)
  {
  list_cmds();
  }

static void perform_if(void)
  {
  long ffhandle;
  int not_flag = false;
  int condition_fulfilled = false;

  if (cmd_arg[0] == '\0')
    goto SyntaxError;

  if (stricmp(cmd_arg, "not") == 0)
    {
    not_flag = true;
    advance_cmd_arg();
    }

  if (stricmp(cmd_arg, "exist") == 0)  // conditional is "exist <filename>"
    {
    char *s;
    finddata_t ff;

    advance_cmd_arg();
    s = strrchr(cmd_arg, '\\');
    if (s != NULL)
      {
      if (stricmp(s,"\\nul") == 0)
        *s = '\0';
      }
    if (file_access(cmd_arg, F_OK) == 0 || file_access(cmd_arg, D_OK) == 0 ||
        findfirst_f(cmd_arg, &ff, 0, &ffhandle) == 0)
      condition_fulfilled = true;
    }
  else if (strnicmp(cmd_args, "errorlevel", 10) == 0) //conditional is "errolevel x"
    {
    char *s;
    unsigned ecomp;

    s = cmd_args+10;
    if (*s != ' ' && *s != '\t' && *s != '=')
      goto SyntaxError;
    while (*s == ' ' || *s == '\t' || *s == '=')
      {
      *s = 'x';
      s++;
      }
    if (sscanf(s, "%u", &ecomp) == 1)
      {
      if (error_level >= ecomp)
        {
        while (*s != ' ' && *s != '\t')
          {
          *s = 'x';
          s++;
          }
        condition_fulfilled = true;
        }
      }
    }
  else // assume the conditional is in the form " xxxxxxxx  ==  yyyyy "
    {  //                                      op1^ op1end^ ^eq ^op2 ^op2end
    int len;
    char *op1, *op1end, *eq, *op2, *op2end;

    op1 = cmd_args;
    while (*op1 == ' ' || *op1 == '\t')
      op1++;

    op1end = op1;
    while (*op1end != ' ' && *op1end != '\t' && *op1end != '\0' && *op1end != '=')
      op1end++;
    if (*op1end == '\0')
      goto SyntaxError;
    len = op1end - op1;
    if (len == 0)
      goto SyntaxError;

    eq = op1end;
    while (*eq != '\0' && *eq != '=')
      eq++;
    if (*eq != '=')
      goto SyntaxError;
    eq++;
    if (*eq != '=')
      goto SyntaxError;
    while (*eq == '=')
      eq++;

    op2 = eq;
    while (*op2 == ' ' || *op2 == '\t')
      op2++;

    op2end = op2;
    while (*op2end != ' ' && *op2end != '\t' && *op2end != '\0')
      op2end++;
    if (op2 == op2end)
      goto SyntaxError;

    if (len == (op2end - op2))
      {
      if (strnicmp(op1, op2, len) == 0)
        condition_fulfilled = true;
      }

    while (op1 != op2end)
      {
      *op1 = 'x';
      op1++;
      }

    }

  advance_cmd_arg();

  if ((not_flag && !condition_fulfilled) ||
      (!not_flag && condition_fulfilled))
    {
    strcpy(cmd, cmd_arg);
    advance_cmd_arg();
    }
  else
    cmd[0] = '\0';

  return;

SyntaxError:
  cputs("Syntax error\r\n");
  reset_batfile_call_stack();
  cmd_line[0] = '\0';
  parse_cmd_line();  // this clears cmd[], cmd_arg[], cmd_switch[], and cmd_args[]
  return;
  }

static void perform_md(const char *arg)
  {
  while (*cmd_switch)  // skip switches
    advance_cmd_arg();
  if (*arg)
    {
    if (file_access(arg, D_OK) != 0 && mkdir(arg) != 0)
      {
      cprintf("Could not create directory - %s\r\n", arg);
      reset_batfile_call_stack();
      }
    }
  else
    {
    cputs("Required parameter missing");
    reset_batfile_call_stack();
    }
  }

static void perform_more(const char *arg)
  {
  	int c;
    while ((c = getchar()) != EOF)
      putchar(c);
  }

static void perform_move(const char *arg)
  {
  general_file_transfer(FILE_XFER_MOVE);
  }

static void perform_null_cmd(void)
  {
  }

static void perform_path(const char *arg)
  {
  int off = 0;
  if (*cmd_args == '\0')
    {
    char *pathvar = getenv("PATH");
    printf("PATH=");
    if (pathvar == NULL)
      puts("");
    else
      puts(pathvar);
    }
  else
    {
    if (*cmd_args == '=')    /* support PATH= syntax */
      off++;
    memmove(cmd_args+5, cmd_args + off, strlen(cmd_args)+1);
    memcpy(cmd_args, "PATH=", 5);
    perform_set(cmd_args);
    }
  }

static void perform_pause(const char *arg)
  {
  cputs("Press any key to continue . . .\r\n");
  getch();
  }

static void perform_popd(const char *arg)
  {
  if (pushd_stack_level == 0)
    {
    printf("pushd stack empty\n");
    reset_batfile_call_stack();
    return;
    }
  if (chdir(pushd_stack[--pushd_stack_level]) != 0)
    {
    cprintf("Directory does not exist - %s\r\n",arg);
    reset_batfile_call_stack();
    return;
    }
  }

static void perform_prompt(const char *arg)
  {
  if (!arg[0])
    {
    char *promptvar = getenv("PROMPT");
    if (promptvar)
      printf("%s\n", promptvar);
    return;
    }
  memmove(cmd_args+7, cmd_args, strlen(cmd_args)+1);
  memcpy(cmd_args, "PROMPT=", 7);
  perform_set(arg);
  }

static void perform_pushd(const char *arg)
  {
  if (pushd_stack_level >= MAX_STACK_LEVEL)
    {
    printf("pushd stack overflow\n");
    reset_batfile_call_stack();
    return;
    }
  getcwd(pushd_stack[pushd_stack_level], MAXPATH);
  if (arg && arg[0])
    {
    if (chdir(arg) != 0)
      {
      cprintf("Directory does not exist - %s\r\n",arg);
      reset_batfile_call_stack();
      return;
      }
    }
  pushd_stack_level++;
  }

static void perform_rd(const char *arg)
  {
  while (*cmd_switch)  // skip switches
    advance_cmd_arg();
  if (*cmd_arg)
    {
    if (rmdir(arg) != 0)
      {
      cprintf("Could not remove directory - %s\r\n", arg);
      reset_batfile_call_stack();
      }
    }
  else
    {
    cputs("Required parameter missing");
    reset_batfile_call_stack();
    }
  }

static void perform_rename(const char *arg)
  {
  long ffhandle;
  int ffrc;
  int first, zfill;
  finddata_t ff;
  unsigned attrib = FA_RDONLY+FA_ARCH+FA_SYSTEM;
  char from_path[MAXPATH] = "", to_path[MAXPATH] = "";
  char from_drive[MAXDRIVE], to_drive[MAXDRIVE];
  char from_dir[MAXDIR], to_dir[MAXDIR];
  char from_name[MAXFILE], to_name[MAXFILE], new_to_name[MAXFILE];
  char from_ext[MAXEXT], to_ext[MAXEXT], new_to_ext[MAXEXT];
  char full_from_filespec[MAXPATH], full_to_filespec[MAXPATH];
  char *w;

  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      if (*from_path == '\0')
        {
        strncpy(from_path, arg, MAXPATH);
        from_path[MAXPATH-1] = '\0';
        conv_unix_path_to_ms_dos(from_path);
        }
      else if (*to_path == '\0')
        {
        strncpy(to_path, arg, MAXPATH);
        to_path[MAXPATH-1] = '\0';
        conv_unix_path_to_ms_dos(to_path);
        }
      else
        {
        cprintf("Too many parameters - %s\r\n", cmd_args);
        reset_batfile_call_stack();
        return;
        }
      }
    else
      {
      cprintf("Invalid switch - %s\r\n", cmd_args);
      reset_batfile_call_stack();
      return;
      }
    advance_cmd_arg();
    }

  _fixpath(from_path, full_from_filespec);
  conv_unix_path_to_ms_dos(full_from_filespec);
  fnsplit(full_from_filespec, from_drive, from_dir, from_name, from_ext);
  if (has_wildcard(from_dir) || has_trailing_slash(from_path))
    {
    cprintf("Invalid parameter - %s\r\n", from_path);
    reset_batfile_call_stack();
    return;
    }

  _fixpath(to_path, full_to_filespec);
  conv_unix_path_to_ms_dos(full_to_filespec);
  fnsplit(full_to_filespec, to_drive, to_dir, to_name, to_ext);
  if (stricmp(from_drive, to_drive) != 0 ||
      stricmp(from_dir, to_dir) != 0 ||
      has_trailing_slash(to_path))
    {
    cprintf("Invalid parameter - %s\r\n", to_path);
    reset_batfile_call_stack();
    return;
    }

  first = true;
  for (;;)
    {
    if (first)
      {
      if ((ffrc = findfirst_f(full_from_filespec, &ff, attrib, &ffhandle)) != 0)
        {
        cprintf("File not found - %s\r\n", from_path);
//        reset_batfile_call_stack();
        return;
        }
      first = false;
      }
    else
      {
      if ((ffrc = findnext_f(&ff, ffhandle)) != 0)
        break;
      }

    strcpy(full_from_filespec, from_drive);
    strcat(full_from_filespec, from_dir);
    strcat(full_from_filespec, FINDDATA_T_FILENAME(ff));
    conv_unix_path_to_ms_dos(full_from_filespec);
    fnsplit(full_from_filespec, NULL, NULL, from_name, from_ext);
    for (zfill = strlen(from_name); zfill < MAXFILE; zfill++)
      from_name[zfill] = '\0';
    for (zfill = strlen(from_ext); zfill < MAXEXT; zfill++)
      from_ext[zfill] = '\0';

    strcpy(full_to_filespec, from_drive);
    strcat(full_to_filespec, from_dir);
    strcpy(new_to_name, to_name);
    strcpy(new_to_ext, to_ext);
    while ((w = strchr(new_to_name, '?')) != NULL)
      *w = from_name[w-new_to_name];
    if ((w = strchr(new_to_name, '*')) != NULL)
      strcpy(w, &(from_name[w-new_to_name]));
    while ((w = strchr(new_to_ext, '?')) != NULL)
      *w = from_ext[w-new_to_ext];
    if ((w = strchr(new_to_ext, '*')) != NULL)
      strcpy(w, &(from_ext[w-new_to_ext]));
    fnmerge(full_to_filespec, to_drive, to_dir, new_to_name, new_to_ext);
    conv_unix_path_to_ms_dos(full_to_filespec);
    if (stricmp(full_from_filespec, full_to_filespec) != 0)
      {
      conv_unix_path_to_ms_dos(new_to_name);
      conv_unix_path_to_ms_dos(new_to_ext);
      if (rename(full_from_filespec, full_to_filespec) == 0)
        printf("%s renamed to %s%s\n", full_from_filespec, new_to_name, new_to_ext);
      else
        {
        cprintf("Unable to rename %s to %s%s\n", full_from_filespec, new_to_name, new_to_ext);
        reset_batfile_call_stack();
        return;
        }
      }
    }
  }

static void perform_set(const char *arg)
  {
  const char *var_name;
  char *vname;
  int err;

  if (*arg == '\0')
    {
    int i = 0;
    while (environ[i])
      printf("%s\n", environ[i++]);
    }
  else
    {
    char *s;
    var_name = cmd_args;
    if (strlen(var_name) == 0)
      {
      cputs("Syntax error\r\n");
      reset_batfile_call_stack();
      return;
      }
    vname = strdup(var_name);
    s = strchr(vname, '=');
    if (s)
      {
      *s = '\0';
      s++;
      }
    strupr(vname);
    if (!s || !*s)
      err = unsetenv(vname);
    else
      err = setenv(vname, s, 1);
    free(vname);
    if (err != 0)
      {
      cprintf("Error setting environment variable - %s\r\n", var_name);
      reset_batfile_call_stack();
      return;
      }
    }
  }

static void perform_shift(const char *arg)
  {
  int i;
  for (i = 0; i < MAX_BAT_ARGS - 1; i++)
    {
    strcpy(bat_arg[stack_level][i], bat_arg[stack_level][i + 1]);
    /* check _after_ copy to zero out last entry */
    if (!bat_arg[stack_level][i + 1][0])
      break;
    }
  }

static void perform_time(const char *arg)
  {
  time_t t = time(NULL);
  struct tm *loctime = localtime (&t);
  char ampm;
  int hour;

  hour = loctime->tm_hour;
  if (hour < 12)
    ampm = 'a';
  else
    ampm = 'p';
  if (hour > 12)
    hour -= 12;
  if (hour == 0)
    hour = 12;

  printf("Current time is %2d:%02d:%02d.%02d%c\n",
         hour, loctime->tm_min, loctime->tm_sec, 0, ampm);

  }

static void perform_type(const char *arg)
  {
  FILE *textfile;
  char filespec[MAXPATH] = "";
  int c;

  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      if (*filespec == '\0')
        {
        strncpy(filespec, arg, MAXPATH);
        filespec[MAXPATH-1] = '\0';
        conv_unix_path_to_ms_dos(filespec);
        }
      else
        {
        cprintf("Too many parameters - %s\r\n", cmd_args);
        reset_batfile_call_stack();
        return;
        }
      }
    else
      {
      }
    advance_cmd_arg();
    }

  textfile = fopen(filespec,"rt");
  if (textfile != NULL)
    {
    while ((c=fgetc(textfile)) != EOF)
      putchar(c);
    fclose(textfile);
    }
  else
    {
    cprintf("Unable to open file - %s\r\n", filespec);
    reset_batfile_call_stack();
    }
  }

static int is_blank(const char cc)
  {
  if (cc == 32 || cc == 9 || cc == 13 || cc == 10)
    {
    return 1;
    }
  else
    {
    return 0;
    }
  }

static void perform_ver(const char *arg)
  {
  int is_r = 0;
  while (*arg != '\0')
    {
    if (*cmd_switch == '\0') // if not a command switch ...
      {
      cprintf("Invalid parameter - %s\r\n", cmd_args);
      reset_batfile_call_stack();
      return;
      }
    else
      {
      if (stricmp(cmd_switch,"/r")==0)
        {
        is_r = 1;
        }
      else
        {
        cprintf("Invalid switch - %s\r\n", cmd_switch);
        reset_batfile_call_stack();
        return;
        }
      }
    advance_cmd_arg();
    }


  printf("comcom32 v0.1\n");
  if (strlen(revisionid))
    {
    printf(" Source Control Revision ID: %s\n", revisionid);
    }
  if (is_r)
    {
    const int buffersize = 256;
    int ver_major, ver_minor, true_ver_major, true_ver_minor, oem_id;
    unsigned ver_string, ii;
    char *pc, *buffer = malloc(buffersize);
    __dpmi_regs r = {};
    r.x.ax = 0x3000;
    __dpmi_int(0x21, &r);
    ver_major = r.h.al;
    ver_minor = r.h.ah;
    oem_id = r.h.bh;
    printf("\nReported DOS version (Int21.3000): %u.%02u OEM: %02Xh\n",
		ver_major, ver_minor, oem_id);
    r.x.ax = 0x3306;
    r.x.bx = 0;
    __dpmi_int(0x21, &r);
    if (! r.x.bx)
      {
      printf("Reported true DOS version (Int21.3306): (none)\n");
      }
    else
      {
      true_ver_major = r.h.bl;
      true_ver_minor = r.h.bh;
      printf("Reported true DOS version (Int21.3306): %u.%02u\n",
		true_ver_major, true_ver_minor);
      }
    r.x.ax = 0x33FF;
    r.x.dx = 0;
    __dpmi_int(0x21, &r);
    if (! r.x.dx)
      {
      printf("Version string (Int21.33FF): (none)\n");
      }
    else
      {
      if (! buffer)
        {
        printf("Version string (Int21.33FF): (buffer allocation failure)\n");
        }
      else
        {
        ver_string = (r.x.dx << 4) + r.x.ax;
        dosmemget(ver_string, buffersize - 1, buffer);
        buffer[buffersize - 1] = 0;
        pc = buffer;
        while (is_blank(*pc))
          {
          ++pc;
          }
        ii = strlen(pc);
        while (ii > 1 && is_blank(pc[ii - 1]))
          {
          --ii;
          }
        pc[ii] = 0;
        printf("Version string (Int21.33FF): %s\n", pc);
        }
      }
    free(buffer);
    }
  }

static void perform_cls(const char *arg)
  {
  clrscr();
  }

#if 0
static void perform_unimplemented_cmd(void)
  {
  cputs("Command not implemented\r\n");
  reset_batfile_call_stack();
  }
#endif

//////////////////////////////////////////////////////////////////////////////////

struct built_in_cmd
  {
  const char *cmd_name;
  void (*cmd_fn)(const char *);
  const char *opts;
  const char *help;
  };

static struct built_in_cmd cmd_table[] =
  {
    {"attrib", perform_attrib, "", "set file attributes"},
    {"call", perform_call, "", "call batch file"},
    {"cd", perform_cd, "", "change directory"},
    {"chdir", perform_cd, "", "change directory"},
    {"choice", perform_choice, "", "choice prompt sets ERRORLEVEL"},
    {"cls", perform_cls, "", "clear screen"},
    {"copy", perform_copy, "", "copy file"},
    {"date", perform_date, "", "display date"},
    {"del", perform_delete, "", "delete file"},
    {"deltree", perform_deltree, "", "delete directory recursively"},
    {"erase", perform_delete, "", "delete file"},
    {"dir", perform_dir, "", "directory listing"},
    {"echo.", perform_echo_dot, "", "terminal output"},  // before normal echo
    {"echo", perform_echo, "", "terminal output"},
    {"exit", perform_exit, "", "exit from interpreter"},
    {"for", perform_for, "", "FOR loop"},
    {"goto", perform_goto, "", "move to label"},
    {"help", perform_help, "", "display this help"},
    {"lh", perform_loadhigh, "", "load program to UMB"},
    {"license", perform_license, "", "show copyright information"},
    {"loadfix", perform_loadfix, "", "fix \"packed file is corrupt\""},
    {"loadhigh", perform_loadhigh, "", "load program to UMB"},
    {"md", perform_md, "", "create directory"},
    {"mkdir", perform_md, "", "create directory"},
    {"move", perform_move, "", "move file"},
    {"more", perform_more, "", "scroll-pause long output"},
    {"path", perform_path, "", "set search path"},
    {"pause", perform_pause, "", "wait for a keypress"},
    {"popd", perform_popd, "", "pop dir from stack and cd"},
    {"prompt", perform_prompt, "", "customize prompt string"},
    {"pushd", perform_pushd, "", "push cwd to stack and cd"},
    {"rd", perform_rd, "", "remove directory"},
    {"rmdir", perform_rd, "", "remove directory"},
    {"rename", perform_rename, "", "rename with wildcards"},
    {"ren", perform_rename, "", "rename with wildcards"},
    {"set", perform_set, "", "set/unset environment variables"},
    {"shift", perform_shift, "", "shift arguments"},
    {"time", perform_time, "", "display time"},
    {"type", perform_type, "", "display file content"},
    {"ver", perform_ver, " [/r]", "display version"},
    {"xcopy", perform_xcopy, "", "copy large file"},
  };

static void list_cmds(void)
  {
  int i, j;

  printf("\tAvailable commands:\n");
  for (i = 0; i < CMD_TABLE_COUNT; i++) {
    int num = printf("%s%s - %s", cmd_table[i].cmd_name, cmd_table[i].opts,
        cmd_table[i].help);
    if (!(i & 1))
      {
      for (j = num; j < 40; j++)
        printf(" ");
      }
      else
        printf("\n");
  }
  printf("\n");
  }

static bool is_valid_DOS_char(int c)
{
  unsigned char u=(unsigned char)c; /* convert to ascii */
  if (!u) return false;
  if (u >= 128 || isalnum(u)) return true;

  /* now we add some extra special chars  */
  if(strchr("_^$~!#%&-{}()@'`",c)!=0) return true; /* general for
                                                    any codepage */
  /* no match is found, then    */
  return false;
}

static void parse_cmd_line(void)
  {
  int c, cmd_len, *pipe_count_addr;
  char *extr, *dest, *saved_extr, *delim;
  char new_cmd_line[MAX_CMD_BUFLEN], *end;
  const char *v;

  // substitute in variable values before parsing
  extr = strchr(cmd_line, '%');
  if (extr != NULL)
    {
    dest = new_cmd_line;
    extr = cmd_line;
    v = NULL;
    while ((v != NULL || *extr != '\0') && dest < new_cmd_line+MAX_CMD_BUFLEN-1)
      {
      if (v == NULL)
        {
        if (*extr == '%')
          {
          extr++;
          if (*extr == '0')                   //  '%0'
            {
            extr++;
            v = bat_file_path[stack_level];
            continue;
            }
          if (*extr >= '1' && *extr <= '9')       //  '%1' to '%9'
            {
            v = bat_arg[stack_level][(*extr)-'1'];
            extr++;
            continue;
            }
          end = strchr(extr, '%');             // find ending '%'
          delim = strchr(extr, ' ');
          if (end == NULL || (delim && end > delim))  // if '%' found, but no ending '%' ...
            {
            if (*extr && *extr == for_var)
              {
              v = for_val;
              extr++;
              continue;
              }
            else
              {
              *dest = '%';
              dest++;
              continue;
              }
            }
          else                              // else ending '%' is found too
            {
            if (extr == end)                   //   if "%%", replace with single '%'
              {
              extr++;
              *dest = '%';
              dest++;
              continue;
              }
            else
              {
              *end = '\0';
              strupr(extr);
              v = getenv(extr);
              extr = end + 1;
              }
            }
          }
        else
          {
          *dest = *extr;
          dest++;
          extr++;
          }
        }
      else
        {
        if (*v != '\0')
          {
          *dest = *v;
          dest++;
          v++;
          }
        if (*v == '\0')
          v = NULL;
        }
      }
    *dest = '\0';
    strcpy(cmd_line, new_cmd_line);
    }

  // extract pipe specs....
  pipe_file[STDIN_INDEX][0] = '\0';   //  <
  pipe_file_redir_count[STDIN_INDEX] = 0;   // count of '<' characters

  pipe_file[STDOUT_INDEX][0] = '\0';  //  > or >>
  pipe_file_redir_count[STDOUT_INDEX] = 0;   // count of '>' characters

  pipe_to_cmd[0] = '\0';      // |
  pipe_to_cmd_redir_count = 0; // count of '|' characters

  extr = cmd_line;
  while (*extr != '\0')
    {
    c = *extr;
    switch (*extr)
      {
      case '<':
        dest =  pipe_file[STDIN_INDEX];
        pipe_count_addr = &(pipe_file_redir_count[STDIN_INDEX]);
        break;
      case '>':
        dest = pipe_file[STDOUT_INDEX];
        pipe_count_addr = &(pipe_file_redir_count[STDOUT_INDEX]);
        break;
      case '|':
        dest = pipe_to_cmd;
        pipe_count_addr = &pipe_to_cmd_redir_count;
        break;
      default:
        c = 0;
        break;
      }

    if (c == 0)
      extr++;
    else
      {
      // count redirection characters
      saved_extr = extr;
      while (*extr == c)
        {
        (*pipe_count_addr)++;
        extr++;
        }

      // skip over spaces
      while (*extr == ' ' || *extr == '\t')
        extr++;

      // extract pipe destinations
      if (c == '|')     // "pipe to" command
        {
        while (*extr != '\0')
          {
          *dest = *extr;
          dest++;
          extr++;
          }
        }
      else             // pipe in or out file
        {
        while (*extr != ' ' && *extr != '\t' && *extr != '\0')
          {
          *dest = *extr;
          dest++;
          extr++;
          }
        }
      *dest = '\0';

      // snip out pipe spec from the cmd_line[] string
      memmove(saved_extr, extr, strlen(extr)+1);
      extr = saved_extr;
      }
    }
  conv_unix_path_to_ms_dos(pipe_file[STDIN_INDEX]);
  conv_unix_path_to_ms_dos(pipe_file[STDOUT_INDEX]);

  // done with variables and pipes -- now, skip leading spaces
  extr = cmd_line;
  while (*extr == ' ' || *extr == '\t')
    extr++;
  if (*extr == '\0')
    {
    cmd[0] = '\0';
    cmd_arg[0] = '\0';
    cmd_switch[0] = '\0';
    cmd_args[0] = '\0';
    return;
    }

  // extract built-in command if command line contains one
  for (c = 0; c < CMD_TABLE_COUNT; c++)
    {
    cmd_len = strlen(cmd_table[c].cmd_name);
    if (strnicmp(extr, cmd_table[c].cmd_name, cmd_len) == 0)
      {
      delim = extr+cmd_len;
      if (!is_valid_DOS_char(delim[0]) || !is_valid_DOS_char(delim[-1]))
        {
        // ok, we have a built-in command; extract it
        strcpy(cmd, cmd_table[c].cmd_name);
        extr = delim;
        break;
        }
      }
    }

  // if not built-in command, extract as an external command
  if (c >= CMD_TABLE_COUNT)
    {
    dest = cmd;
    while (*extr != ' ' && *extr != '\t' && *extr != '/' && *extr != '\0')
      {
      *dest = *extr;
      dest++;
      extr++;
      }
    *dest = '\0';
    }

  // extract the rest as arguments
  extract_args(extr);
  return;
  }

static void exec_cmd(void)
  {
  int c;
  int pipe_index, pipe_fno[2], old_std_fno[2], redir_result[2];

  for (pipe_index = 0; pipe_index < 2; pipe_index++)
    {
    pipe_fno[pipe_index] = -1;
    old_std_fno[pipe_index] = -1;
    redir_result[pipe_index] = -1;
    }

  if (pipe_to_cmd_redir_count > 0)
    {
      pipe(pipe_fno);
    /* cputs("Piping between 2 commands is not supported\r\n");
       reset_batfile_call_stack();
       goto Exit; */
    }
  else // open the pipe file
    {
    if (pipe_file_redir_count[STDIN_INDEX] > 0)
      pipe_fno[STDIN_INDEX] = open(pipe_file[STDIN_INDEX], O_TEXT|O_RDONLY, S_IRUSR);

    if (pipe_file_redir_count[STDOUT_INDEX] > 1)
      pipe_fno[STDOUT_INDEX] = open(pipe_file[STDOUT_INDEX], O_TEXT|O_WRONLY|O_APPEND|O_CREAT, S_IWUSR); // open for append
    else if (pipe_file_redir_count[STDOUT_INDEX] == 1)
      pipe_fno[STDOUT_INDEX] = open(pipe_file[STDOUT_INDEX], O_TEXT|O_WRONLY|O_TRUNC|O_CREAT, S_IWUSR);  // open as new file

      /* check for error
      if (pipe_fno[pipe_index] < 0 ||
          old_std_fno[pipe_index] == -1 ||
          redir_result[pipe_index] == -1)
        {
        if (pipe_index == pipe_index)
          cprintf("Unable to pipe standard input from file - %s\r\n", pipe_file[pipe_index]);
        else
          cprintf("Unable to pipe standard output to file - %s\r\n", pipe_file[pipe_index]);
        reset_batfile_call_stack();
        goto Exit;
        } */
    }

  for (pipe_index = 0; pipe_index < 2; pipe_index++)
    {
      // save a reference to the old standard in/out file handle
      if (pipe_fno[pipe_index] >= 0)
        old_std_fno[pipe_index] = dup(pipe_index);

      // redirect pipe file to standard in/out
      if (pipe_fno[pipe_index] >= 0 && old_std_fno[pipe_index] != -1)
        redir_result[pipe_index] = dup2(pipe_fno[pipe_index], pipe_index);

      // close pipe file handle
      if (pipe_fno[pipe_index] >= 0)
        {
        close(pipe_fno[pipe_index]);
        pipe_fno[pipe_index] = -1;
        }
    }

  while (cmd[0] != '\0')
    {
    if (stricmp(cmd, "if") == 0)
      {
      perform_if();
      continue;
      }
    else if (strnicmp(cmd, "rem", 3) == 0)     // rem statement
      perform_null_cmd();
    else if (cmd[0] == ':')                   // goto label
      perform_null_cmd();
    else if (is_drive_spec(cmd) ||
             is_drive_spec_with_slash(cmd))  // drive letter
      perform_change_drive();
    else
      {
      for (c = 0; c < CMD_TABLE_COUNT; c++)
        {
        if (stricmp(cmd, cmd_table[c].cmd_name) == 0)
          {
          cmd_table[c].cmd_fn(cmd_arg);
          break;
          }
        }
      if (c >= CMD_TABLE_COUNT)
        {
          need_to_crlf_at_next_prompt = true;
          perform_external_cmd(false, cmd);
        }
      }
    cmd[0] = '\0';
    }

  /* Recover the stdout stream */
  if (redir_result[STDOUT_INDEX] != -1) {
    dup2(old_std_fno[STDOUT_INDEX], STDOUT_INDEX);
    close(old_std_fno[STDOUT_INDEX]);
    setbuf(stdout, NULL);
  }

  if (pipe_to_cmd_redir_count > 0)
    {
    for (c = 0; c < CMD_TABLE_COUNT; c++)
        {
        if (stricmp(pipe_to_cmd, cmd_table[c].cmd_name) == 0)
          {
          cmd_table[c].cmd_fn(cmd_arg);
          break;
          }
        }
      if (c >= CMD_TABLE_COUNT)
        perform_external_cmd(false, pipe_to_cmd);
    }

/* Exit: */
  cmd_line[0] = '\0';
  if (redir_result[STDIN_INDEX] != -1) {
    dup2(old_std_fno[STDIN_INDEX], STDIN_INDEX);
    close(old_std_fno[STDIN_INDEX]);
    setbuf(stdin, NULL);
    }
  }

struct MCB {
        char id;                        /* 0 */
        unsigned short owner_psp;       /* 1 */
        unsigned short size;            /* 3 */
        char align8[3];                 /* 5 */
        char name[8];                   /* 8 */
} __attribute__((packed));

static unsigned get_env_size(void)
{
  unsigned short psp = _stubinfo->psp_selector;
  unsigned short env_sel;

  movedata(psp, 0x2c, _my_ds(), (unsigned)&env_sel, 2);
  return __dpmi_get_segment_limit(env_sel) + 1;
}

static void set_env_size(void)
{
  unsigned short psp = _stubinfo->psp_selector;
  unsigned short env_sel;
  unsigned long env_addr;
  struct MCB mcb;
  unsigned old_env_size;
  int err;

  movedata(psp, 0x2c, _my_ds(), (unsigned)&env_sel, 2);
  err = __dpmi_get_segment_base_address(env_sel, &env_addr);
  old_env_size = __dpmi_get_segment_limit(env_sel) + 1;
  if (!err && !(env_addr & 0xf) && env_addr < 0x110000 && old_env_size == 0x10000) {
    dosmemget(env_addr - sizeof(mcb), sizeof(mcb), &mcb);
    __dpmi_set_segment_limit(env_sel, mcb.size * 16 - 1);
  }
}

static unsigned long psp_addr;
static unsigned short orig_psp_seg;

static void set_psp_parent(void)
{
  unsigned short psp = _stubinfo->psp_selector;
  unsigned short psp_seg;
  int err;

  err = __dpmi_get_segment_base_address(psp, &psp_addr);
  if (!err && !(psp_addr & 0xf) && psp_addr < 0x110000) {
    psp_seg = psp_addr >> 4;
    dosmemget(psp_addr + 0x16, 2, &orig_psp_seg);
    dosmemput(&psp_seg, 2, psp_addr + 0x16);
  }
}

static void restore_psp_parent(void)
{
  dosmemput(&orig_psp_seg, 2, psp_addr + 0x16);
}

static void unlink_umb(void)
{
  __dpmi_regs r = {};
  r.x.ax = 0x5803;
  r.x.bx = 0;				/* set UMB link off */
  __dpmi_int(0x21, &r);
}

int main(int argc, char *argv[], char *envp[])

  {
  int a;
  char *cmd_path;
  int disable_autoexec = 0;
  // initialize the cmd data ...

  // Indicate to Dosemu that the DOS has booted. This may be removed after
  // comcom32 supports installable commands properly.
  installable_command_check(argv[0], "");

  // reset fpu
  _clear87();
  _fpreset();
  unlink_umb();		// in case we loaded with shellhigh or lh
  set_env_size();

#ifdef __spawn_leak_workaround
  __spawn_flags &= ~__spawn_leak_workaround;
#endif
  __djgpp_set_ctrl_c(0);    // disable SIGINT on ^C
  _go32_want_ctrl_break(1); // disable SIGINT on ^Break

  // unbuffer stdin and stdout
  setbuf(stdin, NULL);
  setbuf(stdout, NULL);

  // init bat file stack
  reset_batfile_call_stack();

  cmd_path = strdup(argv[0]);
  strupr(cmd_path);
  setenv("COMSPEC", cmd_path, 1);
  free(cmd_path);

  // process arguments
  for (a = 1; a < argc; a++)
    {
    // check for permanent shell
    if (stricmp(argv[a], "/P") == 0)
      {
      shell_permanent = 1;
      }

    if (strnicmp(argv[a], "/E:", 3) == 0)
      {
      unsigned int env_size;
      int seg, sel, old_sel;
      unsigned int old_size = get_env_size();

      if (argv[a][3] == '+')
        env_size = old_size + atoi(argv[a] + 4);
      else
        env_size = atoi(argv[a] + 3);
      env_size &= ~0xf;
      if (env_size > old_size)
        {
        seg = __dpmi_allocate_dos_memory(env_size >> 4, &sel);
        if (seg != -1)
          {
          unsigned short psp = _stubinfo->psp_selector;
          movedata(psp, 0x2c, _my_ds(), (unsigned)&old_sel, 2);
          movedata(_my_ds(), (unsigned)&sel, psp, 0x2c, 2);
          /* copy old content to preserve tail (and maybe COMSPEC) */
          movedata(old_sel, 0, sel, 0, old_size);
          __dpmi_free_dos_memory(old_sel);
          }
        }
      }

    if (stricmp(argv[a], "/D") == 0)
      {
      disable_autoexec = 1;
      }

    // check for command in arguments
    if (stricmp(argv[a], "/K") == 0)
      {
      shell_mode = SHELL_STARTUP_WITH_CMD;
      a++;
      strncat(cmd_line, argv[a], MAX_CMD_BUFLEN-1);
      parse_cmd_line();
      }

    if (stricmp(argv[a], "/C") == 0)
      {
      int cmd_buf_remaining;

      shell_mode = SHELL_SINGLE_CMD;

      // build command from rest of the arguments
      a++;
      cmd_buf_remaining = MAX_CMD_BUFLEN-1;
      while (a < argc)
        {
        strncat(cmd_line, argv[a], cmd_buf_remaining);
        cmd_buf_remaining -= strlen(argv[a]);
        if (cmd_buf_remaining < 0)
          cmd_buf_remaining = 0;
        strncat(cmd_line, " ", cmd_buf_remaining);
        cmd_buf_remaining--;
        if (cmd_buf_remaining < 0)
          cmd_buf_remaining = 0;
        a++;
        }
      parse_cmd_line();
      }
    }

  if (shell_permanent) {
    set_psp_parent();
#if !SYNC_ENV
    /* some progs (Word Perfect) look for COMSPEC in parent env, rather
     * than their own. We need to sync it down to DOS. */
    sync_env();
#endif
  }

  if (shell_permanent && !disable_autoexec)
    {
    unsigned int drive;
    strcpy(bat_file_path[0], "X:\\AUTOEXEC.BAT");  // trigger execution of autoexec.bat
    getdrive(&drive);
    drive += ('A' - 1);
    bat_file_path[0][0] = drive;
    // no arguments for batch file
    }

  // Main command parsing/interpretation/execution loop
  while (!exiting)
    {
    if (cmd_line[0] == '\0')
      {
      if (bat_file_path[stack_level][0] == '\0')
        {
        if (shell_mode == SHELL_SINGLE_CMD)
          {
          perform_exit(NULL);
          continue;
          }
        prompt_for_and_get_cmd();
        }
      else
        get_cmd_from_bat_file();
      }
    exec_cmd();
    }

  if (shell_permanent)
    restore_psp_parent();
  return error_level;
  }
