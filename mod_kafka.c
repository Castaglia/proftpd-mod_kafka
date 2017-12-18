/*
 * ProFTPD - mod_kafka
 * Copyright (c) 2017 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 *
 * -----DO NOT EDIT BELOW THIS LINE-----
 * $Archive: mod_kafka.a $
 * $Libraries: -lrdkafka$
 */

#include "mod_kafka.h"

extern xaset_t *server_list;

/* From response.c */
extern pr_response_t *resp_list, *resp_err_list;

module kafka_module;

int kafka_logfd = -1;
pool *kafka_pool = NULL;

static int kafka_engine = FALSE;

static const char *trace_channel = "kafka";

/* Necessary function prototypes. */
static int kafka_sess_init(void);

/* Configuration handlers
 */

/* usage: KafkaBrokers broker1 ... */
MODRET set_kafkabrokers(cmd_rec *cmd) {
  return PR_HANDLED(cmd);
}

/* usage: KafkaEngine on|off */
MODRET set_kafkaengine(cmd_rec *cmd) {
  int engine = 1;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
}

/* usage: KafkaLog path|"none" */
MODRET set_kafkalog(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* Command handlers
 */

MODRET kafka_log_any(cmd_rec *cmd) {
  return PR_DECLINED(cmd);
}

/* Event handlers
 */

static void kafka_exit_ev(const void *event_data, void *user_data) {
  if (kafka_logfd >= 0) {
    (void) close(kafka_logfd);
    kafka_logfd = -1;
  }
}

#if defined(PR_SHARED_MODULE)
static void kafka_mod_unload_ev(const void *event_data, void *user_data) {
  if (strncmp((const char *) event_data, "mod_kafka.c", 12) == 0) {
    /* Unregister ourselves from all events. */
    pr_event_unregister(&kafka_module, NULL, NULL);

    destroy_pool(kafka_pool);
    kafka_pool = NULL;

    (void) close(kafka_logfd);
    kafka_logfd = -1;
  }
}
#endif

static void kafka_restart_ev(const void *event_data, void *user_data) {
}

static void kafka_sess_reinit_ev(const void *event_data, void *user_data) {
  int res;

  /* A HOST command changed the main_server pointer; reinitialize ourselves. */

  pr_event_unregister(&kafka_module, "core.exit", kafka_exit_ev);
  pr_event_unregister(&kafka_module, "core.session-reinit",
    kafka_sess_reinit_ev);

  (void) close(kafka_logfd);
  kafka_logfd = -1;

  kafka_engine = FALSE;

  res = kafka_sess_init();
  if (res < 0) {
    pr_session_disconnect(&kafka_module, PR_SESS_DISCONNECT_SESSION_INIT_FAILED,
      NULL);
  }
}

static void kafka_shutdown_ev(const void *event_data, void *user_data) {
  int res;

  destroy_pool(kafka_pool);
  kafka_pool = NULL;

  if (kafka_logfd >= 0) {
    (void) close(kafka_logfd);
    kafka_logfd = -1;
  }
}

/* XXX Do we want to support any Controls/ftpctl actions? */

/* Initialization routines
 */

static int kafka_init(void) {

  kafka_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(kafka_pool, MOD_KAFKA_VERSION);

#if defined(PR_SHARED_MODULE)
  pr_event_register(&kafka_module, "core.module-unload", kafka_mod_unload_ev,
    NULL);
#endif
  pr_event_register(&kafka_module, "core.restart", kafka_restart_ev, NULL);
  pr_event_register(&kafka_module, "core.shutdown", kafka_shutdown_ev, NULL);

/* XXX Initialize library, log version? */

  return 0;
}

static int kafka_sess_init(void) {
  config_rec *c;
  int res;

  /* We have to register our HOST handler here, even if KafkaEngine is off,
   * as the current vhost may be disabled BUT the requested vhost may be
   * enabled.
   */
  pr_event_register(&kafka_module, "core.session-reinit",
    kafka_sess_reinit_ev, NULL);

  c = find_config(main_server->conf, CONF_PARAM, "KafkaEngine", FALSE);
  if (c != NULL) {
    kafka_engine = *((int *) c->argv[0]);
  }

  if (kafka_engine == FALSE) {
    return 0;
  }

  pr_event_register(&kafka_module, "core.exit", kafka_exit_ev, NULL);

  c = find_config(main_server->conf, CONF_PARAM, "KafkaLog", FALSE);
  if (c != NULL) {
    char *logname;

    logname = c->argv[0];

    if (strncasecmp(logname, "none", 5) != 0) {
      int xerrno;

      pr_signals_block();
      PRIVS_ROOT
      res = pr_log_openfile(logname, &kafka_logfd, PR_LOG_SYSTEM_MODE);
      xerrno = errno;
      PRIVS_RELINQUISH
      pr_signals_unblock();

      if (res < 0) {
        if (res == -1) {
          pr_log_pri(PR_LOG_NOTICE, MOD_KAFKA_VERSION
            ": notice: unable to open KafkaLog '%s': %s", logname,
            strerror(xerrno));

        } else if (res == PR_LOG_WRITABLE_DIR) {
          pr_log_pri(PR_LOG_NOTICE, MOD_KAFKA_VERSION
            ": notice: unable to open KafkaLog '%s': parent directory is "
            "world-writable", logname);

        } else if (res == PR_LOG_SYMLINK) {
          pr_log_pri(PR_LOG_NOTICE, MOD_KAFKA_VERSION
            ": notice: unable to open KafkaLog '%s': cannot log to a symlink",
            logname);
        }
      }
    }
  }

  kafka_pool = make_sub_pool(session.pool);
  pr_pool_tag(kafka_pool, MOD_KAFKA_VERSION);

  return 0;
}

/* Module API tables
 */

static conftable kafka_conftab[] = {
  { "KafkaBrokers",		set_kafkabrokers,		NULL },
  { "KafkaEngine",		set_kafkaengine,		NULL },
  { "KafkaLog",			set_kafkalog,			NULL },

  { NULL }
};

static cmdtable kafka_cmdtab[] = {
  { LOG_CMD,	C_ANY,	G_NONE,	kafka_log_any,	FALSE, FALSE },
  { LOG_CMD_ERR,C_ANY,	G_NONE,	kafka_log_any,	FALSE, FALSE },

  { 0, NULL }
};

module kafka_module = {
  /* Always NULL */
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "kafka",

  /* Module configuration handler table */
  kafka_conftab,

  /* Module command handler table */
  kafka_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization */
  kafka_init,

  /* Session initialization */
  kafka_sess_init,

  /* Module version */
  MOD_KAFKA_VERSION
};

