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
#include "jot.h"
#include <librdkafka/rdkafka.h>

module kafka_module;

int kafka_logfd = -1;
pool *kafka_pool = NULL;

static pr_table_t *jot_logfmt2json = NULL;
static int kafka_engine = FALSE;

static rd_kafka_t *kafka = NULL;

/* We maintain a map of topic names to topic handles; each LogOnEvent could
 * have a different topic.
 */
static pr_table_t *kafka_topics = NULL;
static rd_kafka_topic_t *kafka_topic = NULL;

#define KAFKA_FLUSH_TIMEOUT_MS		5000
#define KAFKA_POLL_TIMEOUT_MS		500
#define KAFKA_ERRSTR_SIZE		256

static const char *trace_channel = "kafka";

/* Necessary function prototypes. */
static int kafka_sess_init(void);

/* Callbacks */

static void kafka_log_cb(const rd_kafka_t *rk, int level, const char *facility,
    const char *text) {
  pr_trace_msg(trace_channel, 1, "(kafka): [%s:%d] %s", facility, level, text);
}

static void kafka_msg_cb(rd_kafka_t *rk, const rd_kafka_message_t *msg,
    void *event_data) {
  if (msg->err > 0) {
    (void) pr_log_writefile(kafka_logfd, MOD_KAFKA_VERSION,
      "error delivering message: %s", rd_kafka_err2str(msg->err));

  } else {
    pr_trace_msg(trace_channel, 17,
      "message delivered (%lu bytes, partition %d)", (unsigned long) msg->len,
      msg->partition);
  }
}

static int kafka_send_msg(pool *p, rd_kafka_topic_t *topic, char *payload,
    size_t payload_len) {
  int res, flags;

  flags = RD_KAFKA_MSG_F_COPY;

  pr_trace_msg(trace_channel, 17, "producing message (%lu bytes) to topic '%s'",
    (unsigned long) payload_len, rd_kafka_topic_name(topic));

  /* TODO: Handle the RD_KAFKA_RESP_ERR_QUEUE_FULL situation by retrying
   * to produce the message up to N times before giving up.
   */
  res = rd_kafka_produce(topic, RD_KAFKA_PARTITION_UA, flags, payload,
    payload_len, NULL, 0, NULL);
  if (res < 0) {
    (void) pr_log_writefile(kafka_logfd, MOD_KAFKA_VERSION,
      "error producing message to topic '%s': %s",
      rd_kafka_topic_name(topic),
      rd_kafka_err2str(rd_kafka_last_error()));
  }

  /* Always poll afterward, to see if our queue can be flushed. */
  rd_kafka_poll(kafka, KAFKA_POLL_TIMEOUT_MS);

  return 0;
}

static int kafka_topic_destroy_cb(const void *key_data, size_t key_datasz,
    const void *value_data, size_t value_datasz, void *user_data) {
  rd_kafka_topic_t *topic;

  topic = (rd_kafka_topic_t *) value_data;
  rd_kafka_topic_destroy(topic);
  return 0;
}

/* Logging */

static void log_event(config_rec *c, cmd_rec *cmd) {
  pool *tmp_pool;
  int res;
  pr_jot_ctx_t *jot_ctx;
  pr_jot_filters_t *jot_filters;
  pr_json_object_t *json;
  const char *fmt_name = NULL, *topic_name = NULL;
  char *payload = NULL;
  size_t payload_len = 0;
  unsigned char *log_fmt;

  jot_filters = c->argv[0];
  fmt_name = c->argv[1];
  log_fmt = c->argv[2];
  topic_name = c->argv[3];

  if (jot_filters == NULL ||
      fmt_name == NULL ||
      log_fmt == NULL) {
    return;
  }

  tmp_pool = make_sub_pool(cmd->tmp_pool);
  jot_ctx = pcalloc(tmp_pool, sizeof(pr_jot_ctx_t));
  json = pr_json_object_alloc(tmp_pool);
  jot_ctx->log = json;
  jot_ctx->user_data = jot_logfmt2json;

  res = pr_jot_resolve_logfmt(tmp_pool, cmd, jot_filters, log_fmt, jot_ctx,
    pr_jot_on_json, NULL, NULL);
  if (res == 0) {
    payload = pr_json_object_to_text(tmp_pool, json, "");
    payload_len = strlen(payload);
    pr_trace_msg(trace_channel, 8, "generated JSON payload for %s: %.*s",
      (char *) cmd->argv[0], (int) payload_len, payload);

  } else {
    /* EPERM indicates that the message was filtered. */
    if (errno != EPERM) {
      (void) pr_log_writefile(kafka_logfd, MOD_KAFKA_VERSION,
        "error generating JSON formatted log message: %s", strerror(errno));
    }

    payload = NULL;
    payload_len = 0;
  }

  pr_json_object_free(json);

  if (payload_len > 0) {
    rd_kafka_topic_t *topic;

    topic = (rd_kafka_topic_t *) pr_table_get(kafka_topics, topic_name, NULL);
    res = kafka_send_msg(cmd->tmp_pool, topic, payload, payload_len);
    if (res < 0) {
      (void) pr_log_writefile(kafka_logfd, MOD_KAFKA_VERSION,
        "error publishing log message to topic '%s': %s", topic_name,
        strerror(errno));

    } else {
      pr_trace_msg(trace_channel, 17, "published log message to topic '%s'",
        topic_name);
    }
  }

  destroy_pool(tmp_pool);
}

static void log_events(cmd_rec *cmd) {
  config_rec *c;

  c = find_config(CURRENT_CONF, CONF_PARAM, "KafkaLogOnEvent", FALSE);
  while (c != NULL) {
    pr_signals_handle();

    log_event(c, cmd);
    c = find_config_next(c, c->next, CONF_PARAM, "KafkaLogOnEvent", FALSE);
  }
}

/* Configuration handlers
 */

/* usage: KafkaBroker broker1 ... */
MODRET set_kafkabroker(cmd_rec *cmd) {
  register unsigned int i;
  config_rec *c;
  char *brokers;

  if (cmd->argc < 2) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 1, NULL);

  brokers = pstrdup(c->pool, cmd->argv[1]);
  for (i = 2; i < cmd->argc-1; i++) {
    brokers = pstrcat(c->pool, brokers, ",", cmd->argv[i], NULL);
  }

  c->argv[0] = brokers;
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

/* usage: KafkaLogOnEvent "none"|events log-fmt [topic ...] */
MODRET set_kafkalogonevent(cmd_rec *cmd) {
  register unsigned int i;
  config_rec *c, *logfmt_config;
  const char *fmt_name, *rules, *topic = NULL;
  unsigned char *log_fmt = NULL;
  pr_jot_filters_t *jot_filters;

  CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL|CONF_ANON|CONF_DIR);

  if (cmd->argc < 3 ||
      cmd->argc > 5) {

    if (cmd->argc == 2 &&
        strcasecmp(cmd->argv[1], "none") == 0) {
       c = add_config_param(cmd->argv[0], 4, NULL, NULL, NULL, NULL);
       c->flags |= CF_MERGEDOWN;
       return PR_HANDLED(cmd);
    }

    CONF_ERROR(cmd, "wrong number of parameters");
  }

  if ((cmd->argc - 3) % 2 != 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  c = add_config_param(cmd->argv[0], 4, NULL, NULL, NULL, NULL);

  rules = cmd->argv[1];
  jot_filters = pr_jot_filters_create(c->pool, rules,
    PR_JOT_FILTER_TYPE_COMMANDS_WITH_CLASSES,
    PR_JOT_FILTER_FL_ALL_INCL_ALL);
  if (jot_filters == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use events '", rules,
      "': ", strerror(errno), NULL));
  }

  fmt_name = cmd->argv[2];

  for (i = 3; i < cmd->argc; i += 2) {
    if (strcmp(cmd->argv[i], "topic") == 0) {
      topic = cmd->argv[i+1];

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        ": unknown KafkaLogOnEvent parameter: ", cmd->argv[i], NULL));
    }
  }

  /* Make sure that the given LogFormat name is known. */
  logfmt_config = find_config(cmd->server->conf, CONF_PARAM, "LogFormat",
    FALSE);
  while (logfmt_config != NULL) {
    pr_signals_handle();

    if (strcmp(fmt_name, logfmt_config->argv[0]) == 0) {
      log_fmt = logfmt_config->argv[1];
      break;
    }

    logfmt_config = find_config_next(logfmt_config, logfmt_config->next,
      CONF_PARAM, "LogFormat", FALSE);
  }

  if (log_fmt == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "no LogFormat '", fmt_name,
      "' configured", NULL));
  }

  /* If no explicit topic is given, use the LogFormat name as the topic name. */
  if (topic == NULL) {
    topic = fmt_name;
  }

  c->argv[0] = jot_filters;
  c->argv[1] = pstrdup(c->pool, fmt_name);
  c->argv[2] = log_fmt;
  c->argv[3] = pstrdup(c->pool, topic);

  return PR_HANDLED(cmd);
}

/* usage: KafkaProperty name value */
MODRET set_kafkaproperty(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 2);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  (void) add_config_param_str(cmd->argv[0], 2, cmd->argv[1], cmd->argv[2]);
  return PR_HANDLED(cmd);
}

/* Command handlers
 */

MODRET kafka_log_any(cmd_rec *cmd) {
  if (kafka_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  log_events(cmd);
  return PR_DECLINED(cmd);
}

/* Event handlers
 */

static void kafka_exit_ev(const void *event_data, void *user_data) {
  rd_kafka_resp_err_t resp;

  pr_trace_msg(trace_channel, 17, "flushing pending messages");
  resp = rd_kafka_flush(kafka, KAFKA_FLUSH_TIMEOUT_MS);
  if (resp != RD_KAFKA_RESP_ERR_NO_ERROR) {
    pr_trace_msg(trace_channel, 5,
      "error flushing pending messages for %lu ms: %s",
      (unsigned long) KAFKA_FLUSH_TIMEOUT_MS, rd_kafka_err2str(resp));
  }

  pr_table_do(kafka_topics, kafka_topic_destroy_cb, NULL, PR_TABLE_DO_FL_ALL);
  pr_table_empty(kafka_topics);
  pr_table_free(kafka_topics);
  kafka_topics = NULL;
  kafka_topic = NULL;

  rd_kafka_destroy(kafka);
  kafka = NULL;

  if (kafka_logfd >= 0) {
    (void) close(kafka_logfd);
    kafka_logfd = -1;
  }

  destroy_pool(kafka_pool);
  kafka_pool = NULL;
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

/* XXX Do we want to support any Controls/ftpctl actions? */

/* Initialization routines
 */

static int kafka_init(void) {
#if defined(PR_SHARED_MODULE)
  pr_event_register(&kafka_module, "core.module-unload", kafka_mod_unload_ev,
    NULL);
#endif

  if (rd_kafka_version() != RD_KAFKA_VERSION) {
    pr_log_pri(PR_LOG_NOTICE, MOD_KAFKA_VERSION
      ": compiled against '%d.%d.%d', but running using '%s'",
      (RD_KAFKA_VERSION >> 24) & 0xff,
      (RD_KAFKA_VERSION >> 16) & 0xff,
      (RD_KAFKA_VERSION >> 8) & 0xff,
      rd_kafka_version_str());

  } else {
    pr_log_debug(DEBUG2, MOD_KAFKA_VERSION ": using %s",
      rd_kafka_version_str());
  }

  return 0;
}

static int kafka_sess_init(void) {
  config_rec *c;
  int res;
  char *brokers, *topic, errstr[KAFKA_ERRSTR_SIZE];
  size_t builtin_len = 0;
  rd_kafka_conf_t *kafka_conf;
  rd_kafka_conf_res_t conf_res;

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

  c = find_config(main_server->conf, CONF_PARAM, "KafkaBroker", FALSE);
  if (c == NULL) {
    (void) pr_log_writefile(kafka_logfd, MOD_KAFKA_VERSION,
      "no KafkaBroker configured, disabling module");

    (void) close(kafka_logfd);
    kafka_logfd = -1;
    kafka_engine = FALSE;

    return 0;
  }

  brokers = c->argv[0];
  topic = c->argv[1];

  kafka_pool = make_sub_pool(session.pool);
  pr_pool_tag(kafka_pool, MOD_KAFKA_VERSION);

  kafka_conf = rd_kafka_conf_new();

  /* Query the builtin features; we reuse the error string buffer for this. */
  memset(errstr, '\0', sizeof(errstr));
  conf_res = rd_kafka_conf_get(kafka_conf, "builtin.features", errstr,
    &builtin_len);
  if (conf_res == RD_KAFKA_CONF_OK) {
    pr_trace_msg(trace_channel, 12, "builtin features: %.*s",
      (int) builtin_len, errstr);
  }

  memset(errstr, '\0', sizeof(errstr));
  conf_res = rd_kafka_conf_set(kafka_conf, "bootstrap.servers", brokers, errstr,
    sizeof(errstr)-1);
  if (conf_res != RD_KAFKA_CONF_OK) {
    memset(errstr, '\0', sizeof(errstr));
  }

  c = find_config(main_server->conf, CONF_PARAM, "KafkaProperty", FALSE);
  while (c != NULL) {
    const char *name, *value;

    pr_signals_handle();

    name = c->argv[0];
    value = c->argv[1];

    conf_res = rd_kafka_conf_set(kafka_conf, name, value, errstr,
      sizeof(errstr)-1);
    if (conf_res != RD_KAFKA_CONF_OK) {
      memset(errstr, '\0', sizeof(errstr));
    }

    c = find_config_next(c, c->next, CONF_PARAM, "KafkaProperty", FALSE);
  }

  rd_kafka_conf_set_log_cb(kafka_conf, kafka_log_cb);
  rd_kafka_conf_set_dr_msg_cb(kafka_conf, kafka_msg_cb);

  kafka = rd_kafka_new(RD_KAFKA_PRODUCER, kafka_conf, errstr, sizeof(errstr)-1);
  if (kafka == NULL) {
    (void) pr_log_writefile(kafka_logfd, MOD_KAFKA_VERSION,
      "error allocating Kafka handle: %s", errstr);
    (void) rd_kafka_conf_destroy(kafka_conf);

    (void) close(kafka_logfd);
    kafka_logfd = -1;
    kafka_engine = FALSE;
  }

  if (kafka_engine == TRUE) {
    kafka_topics = pr_table_alloc(kafka_pool, 0);

    c = find_config(main_server->conf, CONF_PARAM, "KafkaLogOnEvent", TRUE);
    while (c != NULL) {
      char *topic_name;
      rd_kafka_topic_t *topic;

      pr_signals_handle();

      topic_name = c->argv[3];

      topic = (rd_kafka_topic_t *) pr_table_get(kafka_topics, topic_name, NULL);
      if (topic != NULL) {
        c = find_config_next(c, c->next, CONF_PARAM, "KafkaLogOnEvent", TRUE);
        continue;
      }

      topic = rd_kafka_topic_new(kafka, topic_name, NULL);
      if (topic == NULL) {
        (void) pr_log_writefile(kafka_logfd, MOD_KAFKA_VERSION,
          "error allocating Kafka topic handle for topic '%s': %s", topic_name,
          rd_kafka_err2str(rd_kafka_last_error()));

      } else {
        res = pr_table_add(kafka_topics, topic_name, topic, sizeof(void *));
        if (res < 0) {
          if (errno != EEXIST) {
            (void) pr_log_writefile(kafka_logfd, MOD_KAFKA_VERSION,
              "error stashing handle for topic '%s': %s", topic_name,
              strerror(errno));
          }

        } else {
          pr_trace_msg(trace_channel, 17, "allocated handle for topic '%s'",
            topic_name);
        }
      }

      c = find_config_next(c, c->next, CONF_PARAM, "KafkaLogOnEvent", TRUE);
    }
  }

  if (kafka_engine == TRUE) {
    jot_logfmt2json = pr_jot_get_logfmt2json(kafka_pool);
  }

  return 0;
}

/* Module API tables
 */

static conftable kafka_conftab[] = {
  { "KafkaBroker",		set_kafkabroker,		NULL },
  { "KafkaEngine",		set_kafkaengine,		NULL },
  { "KafkaLog",			set_kafkalog,			NULL },
  { "KafkaLogOnEvent",		set_kafkalogonevent,		NULL },
  { "KafkaProperty",		set_kafkaproperty,		NULL },

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

