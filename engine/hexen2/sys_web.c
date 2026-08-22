#include "quakedef.h"
#include "debuglog.h"
#include "web_perf.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <emscripten/emscripten.h>

#define WEB_HEAP_SIZE	(192 * 1024 * 1024)

cvar_t sys_nostdout = {"sys_nostdout", "0", CVAR_NONE};
cvar_t sys_throttle = {"sys_throttle", "0", CVAR_NONE};
qboolean isDedicated = false;

static DIR *finddir;
static char findpath[MAX_OSPATH];
static char findname[MAX_OSPATH];
static char findpattern[MAX_QPATH];

int Sys_mkdir (const char *path, qboolean crash)
{
	int result = mkdir(path, 0777);
	if (result && errno == EEXIST)
		result = 0;
	if (result && crash)
		Sys_Error("Unable to create %s: %s", path, strerror(errno));
	return result;
}

int Sys_rmdir (const char *path) { return rmdir(path); }
int Sys_unlink (const char *path) { return unlink(path); }
int Sys_rename (const char *oldp, const char *newp) { return rename(oldp, newp); }

int Sys_FileType (const char *path)
{
	struct stat info;
	if (stat(path, &info))
		return FS_ENT_NONE;
	if (S_ISDIR(info.st_mode))
		return FS_ENT_DIRECTORY;
	return S_ISREG(info.st_mode) ? FS_ENT_FILE : FS_ENT_NONE;
}

long Sys_filesize (const char *path)
{
	struct stat info;
	return (!stat(path, &info) && S_ISREG(info.st_mode)) ? (long)info.st_size : -1;
}

int Sys_CopyFile (const char *frompath, const char *topath)
{
	FILE *src = fopen(frompath, "rb");
	FILE *dst;
	byte buffer[8192];
	size_t count;

	if (!src)
		return 1;
	dst = fopen(topath, "wb");
	if (!dst)
	{
		fclose(src);
		return 1;
	}
	while ((count = fread(buffer, 1, sizeof(buffer), src)) != 0)
	{
		if (fwrite(buffer, 1, count, dst) != count)
		{
			fclose(src);
			fclose(dst);
			return 1;
		}
	}
	fclose(src);
	return fclose(dst) != 0;
}

const char *Sys_FindNextFile (void)
{
	struct dirent *entry;
	struct stat info;
	while (finddir && (entry = readdir(finddir)) != NULL)
	{
		if (fnmatch(findpattern, entry->d_name, FNM_PATHNAME))
			continue;
		q_snprintf(findname, sizeof(findname), "%s/%s", findpath, entry->d_name);
		if (!stat(findname, &info) && S_ISREG(info.st_mode))
			return entry->d_name;
	}
	return NULL;
}

const char *Sys_FindFirstFile (const char *path, const char *pattern)
{
	if (finddir)
		Sys_FindClose();
	finddir = opendir(path);
	if (!finddir)
		return NULL;
	q_strlcpy(findpattern, pattern, sizeof(findpattern));
	q_strlcpy(findpath, path, sizeof(findpath));
	return Sys_FindNextFile();
}

void Sys_FindClose (void)
{
	if (finddir)
		closedir(finddir);
	finddir = NULL;
}

int Sys_ListDirectories (const char *path, char dirs[][64], int maxdirs)
{
	DIR *dir = opendir(path);
	struct dirent *entry;
	struct stat info;
	int count = 0;
	char fullpath[MAX_OSPATH];

	if (!dir)
		return 0;
	while (count < maxdirs && (entry = readdir(dir)) != NULL)
	{
		if (entry->d_name[0] == '.')
			continue;
		q_snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
		if (!stat(fullpath, &info) && S_ISDIR(info.st_mode))
			q_strlcpy(dirs[count++], entry->d_name, 64);
	}
	closedir(dir);
	return count;
}

void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length)
{
	(void)startaddr;
	(void)length;
}

void Sys_PrintTerm (const char *text)
{
	if (!sys_nostdout.integer)
		fputs(text, stdout);
}

void Sys_Error (const char *error, ...)
{
	char message[MAX_PRINTMSG];
	va_list args;
	va_start(args, error);
	q_vsnprintf(message, sizeof(message), error, args);
	va_end(args);
	if (host_parms)
		host_parms->errstate++;
	Host_Shutdown();
	EM_ASM({
		const message = UTF8ToString($0);
		dispatchEvent(new CustomEvent('hexenwailquit',
			{ detail: { kind: 'fatal', message } }));
	}, message);
	fprintf(stderr, "FATAL ERROR: %s\n", message);
	exit(1);
}

void Sys_Quit (void)
{
	Host_Shutdown();
	emscripten_cancel_main_loop();
	EM_ASM({
		dispatchEvent(new CustomEvent('hexenwailquit',
			{ detail: { kind: 'quit' } }));
	});
	exit(0);
}

double Sys_DoubleTime (void) { return emscripten_get_now() * 0.001; }
void Sys_Sleep (unsigned long msecs) { (void)msecs; }
void Sys_SendKeyEvents (void) { IN_SendKeyEvents(); }
const char *Sys_ConsoleInput (void) { return NULL; }
char *Sys_GetClipboardData (void) { return NULL; }

char *Sys_DateTimeString (char *buf)
{
	static char storage[24];
	time_t now = time(NULL);
	if (!buf)
		buf = storage;
	strftime(buf, 20, "%m/%d/%Y %H:%M:%S", localtime(&now));
	return buf;
}

static double oldtime;

static void Web_MainFrame (void)
{
	double now = Sys_DoubleTime();
	double delta = now - oldtime;
	if (delta > 0.5)
		delta = 0.1;
	WebPerf_BeginHostFrame();
	Host_Frame(delta);
	WebPerf_EndHostFrame();
	oldtime = now;
}

static quakeparms_t parms;
static char basedir[MAX_OSPATH];

int main (int argc, char **argv)
{
	memset(&parms, 0, sizeof(parms));
	q_strlcpy(basedir, "/persistent", sizeof(basedir));
	parms.basedir = basedir;
	parms.userdir = basedir;
	parms.argc = argc;
	parms.argv = argv;
	parms.memsize = WEB_HEAP_SIZE;
	parms.membase = malloc(parms.memsize);
	host_parms = &parms;

	if (!parms.membase)
		Sys_Error("Unable to allocate the web engine heap");

	COM_ValidateByteorder();
	LOG_Init(&parms);
	Host_Init();
	oldtime = Sys_DoubleTime();
	emscripten_set_main_loop(Web_MainFrame, 0, 1);
	return 0;
}
