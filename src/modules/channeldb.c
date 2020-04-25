/*
 * Stores channel settings for +P channels in a .db file
 * (C) Copyright 2019 Syzop, Gottem and the UnrealIRCd team
 * License: GPLv2
 */

#include "unrealircd.h"

ModuleHeader MOD_HEADER = {
	"channeldb",
	"1.0",
	"Stores and retrieves channel settings for persistent (+P) channels",
	"UnrealIRCd Team",
	"unrealircd-5",
};

#define CHANNELDB_VERSION 100
#define CHANNELDB_SAVE_EVERY 299

#define MAGIC_CHANNEL_START	0x11111111
#define MAGIC_CHANNEL_END	0x22222222

#ifdef DEBUGMODE
 #define BENCHMARK
#endif

#define WARN_WRITE_ERROR(fname) \
	do { \
		sendto_realops_and_log("[channeldb] Error writing to temporary database file " \
		                       "'%s': %s (DATABASE NOT SAVED)", \
		                       fname, strerror(errno)); \
	} while(0)

#define W_SAFE(x) \
	do { \
		if (!(x)) { \
			WARN_WRITE_ERROR(tmpfname); \
			fclose(fd); \
			return 0; \
		} \
	} while(0)

#define IsMDErr(x, y, z) \
	do { \
		if (!(x)) { \
			config_error("A critical error occurred when registering ModData for %s: %s", MOD_HEADER.name, ModuleGetErrorStr((z)->handle)); \
			return MOD_FAILED; \
		} \
	} while(0)

/* Forward declarations */
void channeldb_moddata_free(ModData *md);
void setcfg(void);
void freecfg(void);
int channeldb_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int channeldb_configrun(ConfigFile *cf, ConfigEntry *ce, int type);
EVENT(write_channeldb_evt);
int write_channeldb(void);
int write_channel_entry(FILE *fd, const char *tmpfname, Channel *channel);
int read_channeldb(void);
static void set_channel_mode(Channel *channel, char *modes, char *parameters);

/* Global variables */
static uint32_t channeldb_version = CHANNELDB_VERSION;
struct cfgstruct {
	char *database;
};
static struct cfgstruct cfg;

static int channeldb_loaded = 0;

MOD_TEST()
{
	memset(&cfg, 0, sizeof(cfg));
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, channeldb_configtest);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	MARK_AS_OFFICIAL_MODULE(modinfo);

	LoadPersistentInt(modinfo, channeldb_loaded);

	setcfg();

	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, channeldb_configrun);
	return MOD_SUCCESS;
}

MOD_LOAD()
{
	if (!channeldb_loaded)
	{
		/* If this is the first time that our module is loaded, then read the database. */
		if (!read_channeldb())
		{
			char fname[512];
			snprintf(fname, sizeof(fname), "%s.corrupt", cfg.database);
			if (rename(cfg.database, fname) == 0)
				config_warn("[channeldb] Existing database renamed to %s and starting a new one...", fname);
			else
				config_warn("[channeldb] Failed to rename database from %s to %s: %s", cfg.database, fname, strerror(errno));
		}
		channeldb_loaded = 1;
	}
	EventAdd(modinfo->handle, "channeldb_write_channeldb", write_channeldb_evt, NULL, CHANNELDB_SAVE_EVERY*1000, 0);
	if (ModuleGetError(modinfo->handle) != MODERR_NOERROR)
	{
		config_error("A critical error occurred when loading module %s: %s", MOD_HEADER.name, ModuleGetErrorStr(modinfo->handle));
		return MOD_FAILED;
	}
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	write_channeldb();
	freecfg();
	SavePersistentInt(modinfo, channeldb_loaded);
	return MOD_SUCCESS;
}

void channeldb_moddata_free(ModData *md)
{
	if (md->i)
		md->i = 0;
}

void setcfg(void)
{
	// Default: data/channel.db
	safe_strdup(cfg.database, "channel.db");
	convert_to_absolute_path(&cfg.database, PERMDATADIR);
}

void freecfg(void)
{
	safe_free(cfg.database);
}

int channeldb_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	ConfigEntry *cep;

	// We are only interested in set::channeldb::database
	if (type != CONFIG_SET)
		return 0;

	if (!ce || strcmp(ce->ce_varname, "channeldb"))
		return 0;

	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!cep->ce_vardata) {
			config_error("%s:%i: blank set::channeldb::%s without value", cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
			errors++;
			continue;
		}
		if (!strcmp(cep->ce_varname, "database")) {
			convert_to_absolute_path(&cep->ce_vardata, PERMDATADIR);
			continue;
		}
		config_error("%s:%i: unknown directive set::channeldb::%s", cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
		errors++;
	}

	*errs = errors;
	return errors ? -1 : 1;
}

int channeldb_configrun(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep;

	// We are only interested in set::channeldb::database
	if (type != CONFIG_SET)
		return 0;

	if (!ce || strcmp(ce->ce_varname, "channeldb"))
		return 0;

	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strcmp(cep->ce_varname, "database"))
			safe_strdup(cfg.database, cep->ce_vardata);
	}
	return 1;
}

EVENT(write_channeldb_evt)
{
	write_channeldb();
}

int write_channeldb(void)
{
	char tmpfname[512];
	FILE *fd;
	Channel *channel;
	int cnt = 0;
#ifdef BENCHMARK
	struct timeval tv_alpha, tv_beta;

	gettimeofday(&tv_alpha, NULL);
#endif

	// Write to a tempfile first, then rename it if everything succeeded
	snprintf(tmpfname, sizeof(tmpfname), "%s.tmp", cfg.database);
	fd = fopen(tmpfname, "wb");
	if (!fd)
	{
		WARN_WRITE_ERROR(tmpfname);
		return 0;
	}

	W_SAFE(write_data(fd, &channeldb_version, sizeof(channeldb_version)));

	/* First, count +P channels and write the count to the database */
	for (channel = channels; channel; channel=channel->nextch)
		if (has_channel_mode(channel, 'P'))
			cnt++;
	W_SAFE(write_int64(fd, cnt));

	for (channel = channels; channel; channel=channel->nextch)
	{
		/* We only care about +P (persistent) channels */
		if (has_channel_mode(channel, 'P'))
		{
			if (!write_channel_entry(fd, tmpfname, channel))
				return 0;
		}
	}

	// Everything seems to have gone well, attempt to close and rename the tempfile
	if (fclose(fd) != 0)
	{
		WARN_WRITE_ERROR(tmpfname);
		return 0;
	}

#ifdef _WIN32
	/* The rename operation cannot be atomic on Windows as it will cause a "file exists" error */
	unlink(cfg.database);
#endif
	if (rename(tmpfname, cfg.database) < 0)
	{
		sendto_realops_and_log("[channeldb] Error renaming '%s' to '%s': %s (DATABASE NOT SAVED)", tmpfname, cfg.database, strerror(errno));
		return 0;
	}
#ifdef BENCHMARK
	gettimeofday(&tv_beta, NULL);
	config_status("[channeldb] Benchmark: SAVE DB: %ld microseconds",
		((tv_beta.tv_sec - tv_alpha.tv_sec) * 1000000) + (tv_beta.tv_usec - tv_alpha.tv_usec));
#endif
	return 1;
}

int write_listmode(FILE *fd, const char *tmpfname, Ban *lst)
{
	Ban *l;
	int cnt = 0;

	/* First count and write the list count */
	for (l = lst; l; l = l->next)
		cnt++;
	W_SAFE(write_int32(fd, cnt));

	for (l = lst; l; l = l->next)
	{
		/* The entry, setby, seton */
		W_SAFE(write_str(fd, l->banstr));
		W_SAFE(write_str(fd, l->who));
		W_SAFE(write_int64(fd, l->when));
	}
	return 1;
}

int write_channel_entry(FILE *fd, const char *tmpfname, Channel *channel)
{
	W_SAFE(write_int32(fd, MAGIC_CHANNEL_START));
	/* Channel name */
	W_SAFE(write_str(fd, channel->chname));
	/* Channel creation time */
	W_SAFE(write_int64(fd, channel->creationtime));
	/* Topic (topic, setby, seton) */
	W_SAFE(write_str(fd, channel->topic));
	W_SAFE(write_str(fd, channel->topic_nick));
	W_SAFE(write_int64(fd, channel->topic_time));
	/* Basic channel modes (eg: +sntkl key 55) */
	channel_modes(&me, modebuf, parabuf, sizeof(modebuf), sizeof(parabuf), channel);
	W_SAFE(write_str(fd, modebuf));
	W_SAFE(write_str(fd, parabuf));
	/* Mode lock */
	W_SAFE(write_str(fd, channel->mode_lock));
	/* List modes (bans, exempts, invex) */
	if (!write_listmode(fd, tmpfname, channel->banlist))
		return 0;
	if (!write_listmode(fd, tmpfname, channel->exlist))
		return 0;
	if (!write_listmode(fd, tmpfname, channel->invexlist))
		return 0;
	W_SAFE(write_int32(fd, MAGIC_CHANNEL_END));
	return 1;
}

#define R_SAFE(x) \
	do { \
		if (!(x)) { \
			config_warn("[channeldb] Read error from database file '%s' (possible corruption): %s", cfg.database, strerror(errno)); \
			if (e) \
			{ \
				safe_free(e->banstr); \
				safe_free(e->who); \
				safe_free(e); \
			} \
			return 0; \
		} \
	} while(0)

int read_listmode(FILE *fd, Ban **lst)
{
	uint32_t total;
	uint64_t when;
	int i;
	Ban *e = NULL;

	R_SAFE(read_data(fd, &total, sizeof(total)));

	for (i = 0; i < total; i++)
	{
		e = safe_alloc(sizeof(Ban));
		R_SAFE(read_str(fd, &e->banstr));
		R_SAFE(read_str(fd, &e->who));
		R_SAFE(read_data(fd, &when, sizeof(when)));
		e->when = when;
		e->next = *lst;
		*lst = e;
	}

	return 1;
}
#undef R_SAFE

#define FreeChannelEntry() \
 	do { \
		/* Some of these might be NULL */ \
		safe_free(chname); \
		safe_free(topic); \
		safe_free(topic_nick); \
		safe_free(modes1); \
		safe_free(modes2); \
		safe_free(mode_lock); \
	} while(0)

#define R_SAFE(x) \
	do { \
		if (!(x)) { \
			config_warn("[channeldb] Read error from database file '%s' (possible corruption): %s", cfg.database, strerror(errno)); \
			fclose(fd); \
			FreeChannelEntry(); \
			return 0; \
		} \
	} while(0)

int read_channeldb(void)
{
	FILE *fd;
	uint32_t version;
	int added = 0;
	int i;
	uint64_t count = 0;
	uint32_t magic;
	// Variables for the channels
	// Some of them need to be declared and NULL initialised early due to the macro FreeChannelEntry() being used by R_SAFE() on error
	char *chname = NULL;
	uint64_t creationtime = 0;
	char *topic = NULL;
	char *topic_nick = NULL;
	uint64_t topic_time = 0;
	char *modes1 = NULL;
	char *modes2 = NULL;
	char *mode_lock = NULL;
#ifdef BENCHMARK
	struct timeval tv_alpha, tv_beta;

	gettimeofday(&tv_alpha, NULL);
#endif

	fd = fopen(cfg.database, "rb");
	if (!fd)
	{
		if (errno == ENOENT)
		{
			/* Database does not exist. Could be first boot */
			config_warn("[channeldb] No database present at '%s', will start a new one", cfg.database);
			return 1;
		} else {
			config_warn("[channeldb] Unable to open the database file '%s' for reading: %s", cfg.database, strerror(errno));
			return 0;
		}
	}

	R_SAFE(read_data(fd, &version, sizeof(version)));
	if (version > channeldb_version)
	{
		config_warn("[channeldb] Database '%s' has a wrong version: expected it to be <= %u but got %u instead", cfg.database, channeldb_version, version);
		fclose(fd);
		return 0;
	}

	R_SAFE(read_data(fd, &count, sizeof(count)));

	for (i=1; i <= count; i++)
	{
		// Variables
		chname = NULL;
		creationtime = 0;
		topic = NULL;
		topic_nick = NULL;
		topic_time = 0;
		modes1 = NULL;
		modes2 = NULL;
		mode_lock = NULL;
		
		Channel *channel;
		R_SAFE(read_data(fd, &magic, sizeof(magic)));
		if (magic != MAGIC_CHANNEL_START)
		{
			config_error("[channeldb] Corrupt database (%s) - channel magic start is 0x%x. Further reading aborted.", cfg.database, magic);
			break;
		}
		R_SAFE(read_str(fd, &chname));
		R_SAFE(read_data(fd, &creationtime, sizeof(creationtime)));
		R_SAFE(read_str(fd, &topic));
		R_SAFE(read_str(fd, &topic_nick));
		R_SAFE(read_data(fd, &topic_time, sizeof(topic_time)));
		R_SAFE(read_str(fd, &modes1));
		R_SAFE(read_str(fd, &modes2));
		R_SAFE(read_str(fd, &mode_lock));
		/* If we got this far, we can create/initialize the channel with the above */
		channel = get_channel(&me, chname, CREATE);
		channel->creationtime = creationtime;
		safe_strdup(channel->topic, topic);
		safe_strdup(channel->topic_nick, topic_nick);
		channel->topic_time = topic_time;
		safe_strdup(channel->mode_lock, mode_lock);
		set_channel_mode(channel, modes1, modes2);
		R_SAFE(read_listmode(fd, &channel->banlist));
		R_SAFE(read_listmode(fd, &channel->exlist));
		R_SAFE(read_listmode(fd, &channel->invexlist));
		R_SAFE(read_data(fd, &magic, sizeof(magic)));
		FreeChannelEntry();
		added++;
		if (magic != MAGIC_CHANNEL_END)
		{
			config_error("[channeldb] Corrupt database (%s) - channel magic end is 0x%x. Further reading aborted.", cfg.database, magic);
			break;
		}
	}

	fclose(fd);

	if (added)
		sendto_realops_and_log("[channeldb] Added %d persistent channels (+P)", added);
#ifdef BENCHMARK
	gettimeofday(&tv_beta, NULL);
	ircd_log(LOG_ERROR, "[channeldb] Benchmark: LOAD DB: %ld microseconds",
		((tv_beta.tv_sec - tv_alpha.tv_sec) * 1000000) + (tv_beta.tv_usec - tv_alpha.tv_usec));
#endif
	return 1;
}
#undef FreeChannelEntry
#undef R_SAFE

static void set_channel_mode(Channel *channel, char *modes, char *parameters)
{
	char buf[512];
	char *p, *param;
	int myparc = 1, i;
	char *myparv[64];

	memset(&myparv, 0, sizeof(myparv));
	myparv[0] = raw_strdup(modes);

	strlcpy(buf, parameters, sizeof(buf));
	for (param = strtoken(&p, buf, " "); param; param = strtoken(&p, NULL, " "))
		myparv[myparc++] = raw_strdup(param);
	myparv[myparc] = NULL;

	SetULine(&me); // hack for crash.. set ulined so no access checks.
	do_mode(channel, &me, NULL, myparc, myparv, 0, 0);
	ClearULine(&me); // and clear it again..

	for (i = 0; i < myparc; i++)
		safe_free(myparv[i]);
}
// FIXME: move above function to m_mode and make efunc, available for all modules anyway
