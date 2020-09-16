/*
 * Copyright (C) 2015-2020, Wazuh Inc.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>

#include "../../wrappers/common.h"
#include "../../wrappers/libc/stdio_wrappers.h"
#include "../../wrappers/posix/pthread_wrappers.h"
#include "../../wrappers/posix/unistd_wrappers.h"
#include "../../wrappers/wazuh/shared/debug_op_wrappers.h"
#include "../../wrappers/wazuh/shared/queue_op_wrappers.h"
#include "../../wrappers/wazuh/os_crypto/sha1_op_wrappers.h"
#include "../../wrappers/wazuh/os_net/os_net_wrappers.h"
#include "../../wrappers/wazuh/wazuh_modules/wm_agent_upgrade_wrappers.h"

#include "../../wazuh_modules/wmodules.h"
#include "../../wazuh_modules/agent_upgrade/manager/wm_agent_upgrade_upgrades.h"
#include "../../wazuh_modules/agent_upgrade/manager/wm_agent_upgrade_tasks.h"
#include "../../headers/shared.h"

extern w_queue_t *upgrade_queue;

extern unsigned int upgrade_threads_count;

extern pthread_mutex_t upgrade_threads_mutex;

extern pthread_cond_t upgrade_threads_cond;

typedef struct _test_upgrade_args {
    wm_manager_configs *config;
    wm_agent_task *agent_task;
} test_upgrade_args;

void wm_agent_upgrade_wait_available_thread(unsigned int max_threads);
void wm_agent_upgrade_release_thread();
void* wm_agent_upgrade_start_upgrade(void *arg);
int wm_agent_upgrade_send_wpk_to_agent(const wm_agent_task *agent_task, const wm_manager_configs* manager_configs);
int wm_agent_upgrade_send_lock_restart(int agent_id);
int wm_agent_upgrade_send_open(int agent_id, const char *wpk_file);
int wm_agent_upgrade_send_write(int agent_id, const char *wpk_file, const char *file_path, int chunk_size);
int wm_agent_upgrade_send_close(int agent_id, const char *wpk_file);
int wm_agent_upgrade_send_sha1(int agent_id, const char *wpk_file, const char *file_sha1);
int wm_agent_upgrade_send_upgrade(int agent_id, const char *wpk_file, const char *installer);

// Setup / teardown

static int teardown_string(void **state) {
    char *string = *state;
    os_free(string);
    return 0;
}

static int setup_config(void **state) {
    wm_manager_configs *config = NULL;
    os_calloc(1, sizeof(wm_manager_configs), config);
    *state = config;
    upgrade_queue = queue_init(10);
    return 0;
}

static int teardown_config(void **state) {
    wm_manager_configs *config = *state;
    os_free(config);
    queue_free(upgrade_queue);
    return 0;
}

static int setup_upgrade_args(void **state) {
    test_upgrade_args *args = NULL;
    wm_manager_configs *config = NULL;
    wm_agent_task *agent_task = NULL;
    os_calloc(1, sizeof(test_upgrade_args), args);
    os_calloc(1, sizeof(wm_manager_configs), config);
    agent_task = wm_agent_upgrade_init_agent_task();
    agent_task->agent_info = wm_agent_upgrade_init_agent_info();
    agent_task->task_info = wm_agent_upgrade_init_task_info();
    args->agent_task = agent_task;
    args->config = config;
    state[0] = (void *)args;
    state[1] = (void *)config;
    upgrade_queue = queue_init(10);
    return 0;
}

static int teardown_upgrade_args(void **state) {
    wm_manager_configs *config = state[1];
    os_free(config);
    queue_free(upgrade_queue);
    return 0;
}

static int setup_config_agent_task(void **state) {
    wm_manager_configs *config = NULL;
    wm_agent_task *agent_task = NULL;
    os_calloc(1, sizeof(wm_manager_configs), config);
    agent_task = wm_agent_upgrade_init_agent_task();
    agent_task->agent_info = wm_agent_upgrade_init_agent_info();
    agent_task->task_info = wm_agent_upgrade_init_task_info();
    state[0] = (void *)config;
    state[1] = (void *)agent_task;
    return 0;
}

static int teardown_config_agent_task(void **state) {
    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    os_free(config);
    wm_agent_upgrade_free_agent_task(agent_task);
    return 0;
}

static int setup_config_nodes(void **state) {
    setup_hash_table(NULL);
    wm_manager_configs *config = NULL;
    OSHashNode *node = NULL;
    OSHashNode *node_next = NULL;
    wm_agent_task *agent_task = NULL;
    wm_agent_task *agent_task_next = NULL;
    os_calloc(1, sizeof(wm_manager_configs), config);
    os_calloc(1, sizeof(OSHashNode), node);
    os_calloc(1, sizeof(OSHashNode), node_next);
    agent_task = wm_agent_upgrade_init_agent_task();
    agent_task->agent_info = wm_agent_upgrade_init_agent_info();
    agent_task->task_info = wm_agent_upgrade_init_task_info();
    agent_task_next = wm_agent_upgrade_init_agent_task();
    agent_task_next->agent_info = wm_agent_upgrade_init_agent_info();
    agent_task_next->task_info = wm_agent_upgrade_init_task_info();
    node->data = agent_task;
    node_next->data = agent_task_next;
    node->next = node_next;
    state[0] = (void *)config;
    state[1] = (void *)node;
    return 0;
}

static int teardown_config_nodes(void **state) {
    teardown_hash_table();
    wm_manager_configs *config = (wm_manager_configs *)state[0];
    OSHashNode *node = (OSHashNode *)state[1];
    OSHashNode *node_next = node->next;
    wm_agent_task *agent_task = node->data;
    wm_agent_task *agent_task_next = node_next->data;
    os_free(config);
    wm_agent_upgrade_free_agent_task(agent_task_next);
    wm_agent_upgrade_free_agent_task(agent_task);
    os_free(node_next->key);
    os_free(node_next);
    os_free(node->key);
    os_free(node);
    return 0;
}

static int setup_group(void **state) {
    test_mode = 1;
    return 0;
}

static int teardown_group(void **state) {
    test_mode = 0;
    return 0;
}

// Wrappers

int __wrap_CreateThread(void * (*function_pointer)(void *), void *data) {
    check_expected_ptr(function_pointer);

    test_upgrade_args *args = (test_upgrade_args *)data;
    wm_agent_task *agent_task = args->agent_task;
    wm_manager_configs *config = args->config;

    check_expected(agent_task);
    check_expected(config);

    wm_agent_upgrade_free_agent_task(agent_task);
    os_free(args);

    return 1;
}

// Tests

void test_wm_agent_upgrade_send_command_to_agent_ok(void **state)
{
    int socket = 555;
    char *command = "Command to agent: restart agent now.";
    char *response = "Command received OK.";
    size_t response_size = strlen(response) + 1;

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: 'Command to agent: restart agent now.'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(command));
    expect_string(__wrap_OS_SendSecureTCP, msg, command);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, response);
    will_return(__wrap_OS_RecvSecureTCP, response_size);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'Command received OK.'");

    char *res = wm_agent_upgrade_send_command_to_agent(command, strlen(command));

    *state = res;

    assert_non_null(res);
    assert_string_equal(res, response);
}

void test_wm_agent_upgrade_send_command_to_agent_recv_error(void **state)
{
    int socket = 555;
    char *command = "Command to agent: restart agent now.";
    char *response = "Error.";

    will_return(__wrap_isChroot, 1);

    expect_string(__wrap_OS_ConnectUnixDomain, path, REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: 'Command to agent: restart agent now.'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(command));
    expect_string(__wrap_OS_SendSecureTCP, msg, command);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, response);
    will_return(__wrap_OS_RecvSecureTCP, -1);

    expect_string(__wrap__mterror, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mterror, formatted_msg, "(8111): Error in recv(): 'Success'");

    char *res = wm_agent_upgrade_send_command_to_agent(command, 0);

    *state = res;

    assert_non_null(res);
}

void test_wm_agent_upgrade_send_command_to_agent_sockterr_error(void **state)
{
    int socket = 555;
    char *command = "Command to agent: restart agent now.";
    char *response = "Command received OK.";

    will_return(__wrap_isChroot, 1);

    expect_string(__wrap_OS_ConnectUnixDomain, path, REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: 'Command to agent: restart agent now.'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(command));
    expect_string(__wrap_OS_SendSecureTCP, msg, command);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, response);
    will_return(__wrap_OS_RecvSecureTCP, OS_SOCKTERR);

    expect_string(__wrap__mterror, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mterror, formatted_msg, "(8112): Response size is bigger than expected.");

    char *res = wm_agent_upgrade_send_command_to_agent(command, 0);

    *state = res;

    assert_non_null(res);
    assert_string_equal(res, response);
}

void test_wm_agent_upgrade_send_command_to_agent_connect_error(void **state)
{
    char *command = "Command to agent: restart agent now.";

    will_return(__wrap_isChroot, 1);

    expect_string(__wrap_OS_ConnectUnixDomain, path, REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, OS_SOCKTERR);

    expect_string(__wrap__mterror, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mterror, formatted_msg, "(8114): Cannot connect to '/queue/ossec/request'. Could not reach agent.");

    char *res = wm_agent_upgrade_send_command_to_agent(command, strlen(command));

    *state = res;

    assert_null(res);
}

void test_wm_agent_upgrade_send_lock_restart_ok(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 28;
    char *cmd = "028 com lock_restart -1";
    char *agent_res = "ok ";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '028 com lock_restart -1'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    int res = wm_agent_upgrade_send_lock_restart(agent);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_lock_restart_err(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 28;
    char *cmd = "028 com lock_restart -1";
    char *agent_res = "err Could not restart agent";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '028 com lock_restart -1'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not restart agent'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    int res = wm_agent_upgrade_send_lock_restart(agent);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_open_ok(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 39;
    char *wpk_file = "test.wpk";
    char *cmd = "039 com open wb test.wpk";
    char *agent_res = "ok ";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    int res = wm_agent_upgrade_send_open(agent, wpk_file);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_open_retry_ok(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 39;
    char *wpk_file = "test.wpk";
    char *cmd = "039 com open wb test.wpk";
    char *agent_res1 = "err Could not open file in agent";
    char *agent_res2 = "ok ";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res1) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res1);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res2);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res2) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res2);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    int res = wm_agent_upgrade_send_open(agent, wpk_file);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_open_retry_err(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 39;
    char *wpk_file = "test.wpk";
    char *cmd = "039 com open wb test.wpk";
    char *agent_res = "err Could not open file in agent";

    will_return_count(__wrap_isChroot, 0, 10);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 10);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 10);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 10);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 10);

    expect_value_count(__wrap_OS_SendSecureTCP, sock, socket, 10);
    expect_value_count(__wrap_OS_SendSecureTCP, size, strlen(cmd), 10);
    expect_string_count(__wrap_OS_SendSecureTCP, msg, cmd, 10);
    will_return_count(__wrap_OS_SendSecureTCP, 0, 10);

    expect_value_count(__wrap_OS_RecvSecureTCP, sock, socket, 10);
    expect_value_count(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR, 10);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 20);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not open file in agent'");

    expect_string_count(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res, 10);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID, 10);

    int res = wm_agent_upgrade_send_open(agent, wpk_file);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_write_ok(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 39;
    char *wpk_file = "test.wpk";
    char *file_path = "/var/upgrade/wazuh_agent.wpk";
    int chunk_size = 5;
    char *chunk = "test\n";
    char *cmd = "039 com write 5 test.wpk test\n";
    char *agent_res = "ok ";

    expect_string(__wrap_fopen, path, file_path);
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, chunk);
    will_return(__wrap_fread, chunk_size);

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com write 5 test.wpk test\n'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    will_return(__wrap_fread, chunk);
    will_return(__wrap_fread, chunk_size);

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com write 5 test.wpk test\n'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    will_return(__wrap_fread, chunk);
    will_return(__wrap_fread, 0);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    int res = wm_agent_upgrade_send_write(agent, wpk_file, file_path, chunk_size);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_write_err(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 39;
    char *wpk_file = "test.wpk";
    char *file_path = "/var/upgrade/wazuh_agent.wpk";
    int chunk_size = 5;
    char *chunk = "test\n";
    char *cmd = "039 com write 5 test.wpk test\n";
    char *agent_res1 = "ok ";
    char *agent_res2 = "err Could not write file in agent";

    expect_string(__wrap_fopen, path, file_path);
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, chunk);
    will_return(__wrap_fread, chunk_size);

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com write 5 test.wpk test\n'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res1) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res1);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    will_return(__wrap_fread, chunk);
    will_return(__wrap_fread, chunk_size);

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '039 com write 5 test.wpk test\n'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res2);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res2) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not write file in agent'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res2);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    int res = wm_agent_upgrade_send_write(agent, wpk_file, file_path, chunk_size);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_write_open_err(void **state)
{
    (void) state;

    int agent = 39;
    char *wpk_file = "test.wpk";
    char *file_path = "/var/upgrade/wazuh_agent.wpk";
    int chunk_size = 5;

    expect_string(__wrap_fopen, path, file_path);
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 0);

    int res = wm_agent_upgrade_send_write(agent, wpk_file, file_path, chunk_size);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_close_ok(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 33;
    char *wpk_file = "test.wpk";
    char *cmd = "033 com close test.wpk";
    char *agent_res = "ok ";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '033 com close test.wpk'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    int res = wm_agent_upgrade_send_close(agent, wpk_file);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_close_err(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 33;
    char *wpk_file = "test.wpk";
    char *cmd = "033 com close test.wpk";
    char *agent_res = "err Could not close file in agent";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '033 com close test.wpk'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not close file in agent'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    int res = wm_agent_upgrade_send_close(agent, wpk_file);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_sha1_ok(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 33;
    char *wpk_file = "test.wpk";
    char *file_sha1 = "d321af65983fa412e3a12c312ada12ab321a253a";
    char *cmd = "033 com sha1 test.wpk";
    char *agent_res = "ok d321af65983fa412e3a12c312ada12ab321a253a";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '033 com sha1 test.wpk'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a12c312ada12ab321a253a'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    int res = wm_agent_upgrade_send_sha1(agent, wpk_file, file_sha1);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_sha1_err(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 33;
    char *wpk_file = "test.wpk";
    char *file_sha1 = "d321af65983fa412e3a12c312ada12ab321a253a";
    char *cmd = "033 com sha1 test.wpk";
    char *agent_res = "err Could not calculate sha1 in agent";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '033 com sha1 test.wpk'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not calculate sha1 in agent'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    int res = wm_agent_upgrade_send_sha1(agent, wpk_file, file_sha1);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_sha1_invalid_sha1(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 33;
    char *wpk_file = "test.wpk";
    char *file_sha1 = "d321af65983fa412e3a12c312ada12ab321a253a";
    char *cmd = "033 com sha1 test.wpk";
    char *agent_res = "ok d321af65983fa412e3a21c312ada12ab321a253a";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '033 com sha1 test.wpk'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a21c312ada12ab321a253a'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    expect_string(__wrap__mterror, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mterror, formatted_msg, "(8118): The SHA1 of the file doesn't match in the agent.");

    int res = wm_agent_upgrade_send_sha1(agent, wpk_file, file_sha1);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_upgrade_ok(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 55;
    char *wpk_file = "test.wpk";
    char *installer = "install.sh";
    char *cmd = "055 com upgrade test.wpk install.sh";
    char *agent_res = "ok 0";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '055 com upgrade test.wpk install.sh'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    int res = wm_agent_upgrade_send_upgrade(agent, wpk_file, installer);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_upgrade_err(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 55;
    char *wpk_file = "test.wpk";
    char *installer = "install.sh";
    char *cmd = "055 com upgrade test.wpk install.sh";
    char *agent_res = "err Could not run script in agent";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '055 com upgrade test.wpk install.sh'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err Could not run script in agent'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    int res = wm_agent_upgrade_send_upgrade(agent, wpk_file, installer);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_upgrade_script_err(void **state)
{
    (void) state;

    int socket = 555;
    int agent = 55;
    char *wpk_file = "test.wpk";
    char *installer = "install.sh";
    char *cmd = "055 com upgrade test.wpk install.sh";
    char *agent_res = "ok 2";

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '055 com upgrade test.wpk install.sh'");

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(cmd));
    expect_string(__wrap_OS_SendSecureTCP, msg, cmd);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res) + 1);

    expect_string(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 2'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);

    expect_string(__wrap__mterror, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mterror, formatted_msg, "(8121): Script execution failed in the agent.");

    int res = wm_agent_upgrade_send_upgrade(agent, wpk_file, installer);

    assert_int_equal(res, OS_INVALID);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_linux_ok(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *write_file = "111 com write 5 test.wpk test\n";
    char *close_file = "111 com close test.wpk";
    char *calculate_sha1 = "111 com sha1 test.wpk";
    char *run_upgrade = "111 com upgrade test.wpk upgrade.sh";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_0 = "ok 0";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a12c312ada12ab321a253a";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_0);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_0) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a12c312ada12ab321a253a'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com upgrade test.wpk upgrade.sh'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 6);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_windows_ok(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *write_file = "111 com write 5 test.wpk test\n";
    char *close_file = "111 com close test.wpk";
    char *calculate_sha1 = "111 com sha1 test.wpk";
    char *run_upgrade = "111 com upgrade test.wpk upgrade.bat";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_0 = "ok 0";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a12c312ada12ab321a253a";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("windows", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_0);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_0) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a12c312ada12ab321a253a'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com upgrade test.wpk upgrade.bat'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 6);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_custom_custom_installer_ok(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *write_file = "111 com write 5 test.wpk test\n";
    char *close_file = "111 com close test.wpk";
    char *calculate_sha1 = "111 com sha1 test.wpk";
    char *run_upgrade = "111 com upgrade test.wpk test.sh";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_0 = "ok 0";
    char *agent_res_ok_sha1 = "ok 2c312ada12ab321a253ad321af65983fa412e3a1";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_custom_task *upgrade_custom_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE_CUSTOM;
    upgrade_custom_task = wm_agent_upgrade_init_upgrade_custom_task();
    os_strdup("/tmp/test.wpk", upgrade_custom_task->custom_file_path);
    os_strdup("test.sh", upgrade_custom_task->custom_installer);
    agent_task->task_info->task = upgrade_custom_task;

    // wm_agent_upgrade_validate_wpk_custom
    will_return(__wrap_wm_agent_upgrade_validate_wpk_custom, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    expect_string(__wrap_OS_SHA1_File, fname, "/tmp/test.wpk");
    expect_value(__wrap_OS_SHA1_File, mode, OS_BINARY);
    will_return(__wrap_OS_SHA1_File, "2c312ada12ab321a253ad321af65983fa412e3a1");
    will_return(__wrap_OS_SHA1_File, 0);

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "/tmp/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_0);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_0) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 2c312ada12ab321a253ad321af65983fa412e3a1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com upgrade test.wpk test.sh'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 6);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_custom_default_installer_ok(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *write_file = "111 com write 5 test.wpk test\n";
    char *close_file = "111 com close test.wpk";
    char *calculate_sha1 = "111 com sha1 test.wpk";
    char *run_upgrade = "111 com upgrade test.wpk upgrade.sh";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_0 = "ok 0";
    char *agent_res_ok_sha1 = "ok 2c312ada12ab321a253ad321af65983fa412e3a1";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_custom_task *upgrade_custom_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE_CUSTOM;
    upgrade_custom_task = wm_agent_upgrade_init_upgrade_custom_task();
    os_strdup("/tmp/test.wpk", upgrade_custom_task->custom_file_path);
    agent_task->task_info->task = upgrade_custom_task;

    // wm_agent_upgrade_validate_wpk_custom
    will_return(__wrap_wm_agent_upgrade_validate_wpk_custom, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    expect_string(__wrap_OS_SHA1_File, fname, "/tmp/test.wpk");
    expect_value(__wrap_OS_SHA1_File, mode, OS_BINARY);
    will_return(__wrap_OS_SHA1_File, "2c312ada12ab321a253ad321af65983fa412e3a1");
    will_return(__wrap_OS_SHA1_File, 0);

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "/tmp/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_0);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_0) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 2c312ada12ab321a253ad321af65983fa412e3a1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com upgrade test.wpk upgrade.sh'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 6);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, 0);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_run_upgrade_err(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *write_file = "111 com write 5 test.wpk test\n";
    char *close_file = "111 com close test.wpk";
    char *calculate_sha1 = "111 com sha1 test.wpk";
    char *run_upgrade = "111 com upgrade test.wpk upgrade.sh";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a12c312ada12ab321a253a";
    char *agent_res_err = "err ";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a12c312ada12ab321a253a'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com upgrade test.wpk upgrade.sh'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_err);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 5);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, WM_UPGRADE_SEND_UPGRADE_ERROR);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_send_sha1_err(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *write_file = "111 com write 5 test.wpk test\n";
    char *close_file = "111 com close test.wpk";
    char *calculate_sha1 = "111 com sha1 test.wpk";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a21c312ada12ab321a253a";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    will_return_count(__wrap_isChroot, 0, 5);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 5);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 5);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 5);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 5);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    expect_string(__wrap__mterror, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mterror, formatted_msg, "(8118): The SHA1 of the file doesn't match in the agent.");

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 10);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a21c312ada12ab321a253a'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 5);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, WM_UPGRADE_SEND_SHA1_ERROR);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_close_file_err(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *write_file = "111 com write 5 test.wpk test\n";
    char *close_file = "111 com close test.wpk";
    char *agent_res_ok = "ok ";
    char *agent_res_err = "err ";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    will_return_count(__wrap_isChroot, 0, 4);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 4);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 4);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 4);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 4);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 8);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_err);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 3);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, WM_UPGRADE_SEND_CLOSE_ERROR);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_write_file_err(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *write_file = "111 com write 5 test.wpk test\n";
    char *agent_res_ok = "ok ";
    char *agent_res_err = "err ";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    will_return_count(__wrap_isChroot, 0, 3);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 3);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 3);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 3);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 3);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 6);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_err);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 2);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, WM_UPGRADE_SEND_WRITE_ERROR);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_open_file_err(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *open_file = "111 com open wb test.wpk";
    char *agent_res_ok = "ok ";
    char *agent_res_err = "err ";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    will_return_count(__wrap_isChroot, 0, 11);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 11);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 11);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 11);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 11);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value_count(__wrap_OS_SendSecureTCP, sock, socket, 10);
    expect_value_count(__wrap_OS_SendSecureTCP, size, strlen(open_file), 10);
    expect_string_count(__wrap_OS_SendSecureTCP, msg, open_file, 10);
    will_return_count(__wrap_OS_SendSecureTCP, 0, 10);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 22);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string_count(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_err, 10);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, 0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID, 10);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, WM_UPGRADE_SEND_OPEN_ERROR);
}

void test_wm_agent_upgrade_send_wpk_to_agent_upgrade_lock_restart_err(void **state)
{
    (void) state;

    int socket = 555;
    char *lock_restart = "111 com lock_restart -1";
    char *agent_res_err = "err ";

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '111'");

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 2);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '111 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_err);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, WM_UPGRADE_SEND_LOCK_RESTART_ERROR);
}

void test_wm_agent_upgrade_send_wpk_to_agent_validate_wpk_err(void **state)
{
    (void) state;

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_validate_wpk
    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_WPK_SHA1_DOES_NOT_MATCH);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, WM_UPGRADE_WPK_SHA1_DOES_NOT_MATCH);
}

void test_wm_agent_upgrade_send_wpk_to_agent_validate_wpk_custom_err(void **state)
{
    (void) state;

    wm_manager_configs *config = state[0];
    wm_agent_task *agent_task = state[1];
    wm_upgrade_custom_task *upgrade_custom_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = 111;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE_CUSTOM;
    upgrade_custom_task = wm_agent_upgrade_init_upgrade_custom_task();
    os_strdup("/tmp/test.wpk", upgrade_custom_task->custom_file_path);
    os_strdup("test.sh", upgrade_custom_task->custom_installer);
    agent_task->task_info->task = upgrade_custom_task;

    // wm_agent_upgrade_validate_wpk_custom
    will_return(__wrap_wm_agent_upgrade_validate_wpk_custom, WM_UPGRADE_WPK_FILE_DOES_NOT_EXIST);

    int res = wm_agent_upgrade_send_wpk_to_agent(agent_task, config);

    assert_int_equal(res, WM_UPGRADE_WPK_FILE_DOES_NOT_EXIST);
}

void test_wm_agent_upgrade_start_upgrade_upgrade_ok(void **state)
{
    (void) state;

    upgrade_threads_count = 5;

    int socket = 555;
    int agent_id = 25;
    char *status = "In progress";

    char *lock_restart = "025 com lock_restart -1";
    char *open_file = "025 com open wb test.wpk";
    char *write_file = "025 com write 5 test.wpk test\n";
    char *close_file = "025 com close test.wpk";
    char *calculate_sha1 = "025 com sha1 test.wpk";
    char *run_upgrade = "025 com upgrade test.wpk upgrade.sh";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_0 = "ok 0";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a12c312ada12ab321a253a";

    test_upgrade_args *args = state[0];
    wm_manager_configs *config = args->config;
    wm_agent_task *agent_task = args->agent_task;
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = agent_id;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    // wm_agent_upgrade_send_wpk_to_agent

    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '025'");

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_0);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_0) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a12c312ada12ab321a253a'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com upgrade test.wpk upgrade.sh'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 6);

    will_return(__wrap_queue_pop_ex_timedwait, OS_INVALID);
    expect_memory(__wrap_queue_pop_ex_timedwait, queue, upgrade_queue, sizeof(upgrade_queue));

    expect_value(__wrap_pthread_cond_signal, cond, &upgrade_threads_cond);

    wm_agent_upgrade_start_upgrade(args);

    assert_int_equal(upgrade_threads_count, 4);
}

void test_wm_agent_upgrade_start_upgrade_upgrade_legacy_ok(void **state)
{
    (void) state;

    upgrade_threads_count = 5;

    int socket = 555;
    int agent_id = 25;
    char *status = "Legacy";

    char *lock_restart = "025 com lock_restart -1";
    char *open_file = "025 com open wb test.wpk";
    char *write_file = "025 com write 5 test.wpk test\n";
    char *close_file = "025 com close test.wpk";
    char *calculate_sha1 = "025 com sha1 test.wpk";
    char *run_upgrade = "025 com upgrade test.wpk upgrade.sh";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_0 = "ok 0";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a12c312ada12ab321a253a";

    test_upgrade_args *args = state[0];
    wm_manager_configs *config = args->config;
    wm_agent_task *agent_task = args->agent_task;
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = agent_id;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    os_strdup("v3.13.1", upgrade_task->custom_version);
    agent_task->task_info->task = upgrade_task;

    cJSON *task_request_status = cJSON_CreateObject();

    cJSON_AddStringToObject(task_request_status, "module", "upgrade_module");
    cJSON_AddStringToObject(task_request_status, "command", "upgrade_update_status");
    cJSON_AddNumberToObject(task_request_status, "agent", agent_id);
    cJSON_AddStringToObject(task_request_status, "status", status);

    cJSON *task_response_status = cJSON_CreateObject();

    cJSON_AddStringToObject(task_response_status, "error", WM_UPGRADE_SUCCESS);
    cJSON_AddStringToObject(task_response_status, "data", upgrade_error_codes[WM_UPGRADE_SUCCESS]);
    cJSON_AddNumberToObject(task_response_status, "agent", agent_id);
    cJSON_AddStringToObject(task_response_status, "status", status);

    // wm_agent_upgrade_send_wpk_to_agent

    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '025'");

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_0);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_0) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a12c312ada12ab321a253a'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com upgrade test.wpk upgrade.sh'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 6);

    // wm_agent_upgrade_compare_versions

    expect_string(__wrap_wm_agent_upgrade_compare_versions, version1, "v3.13.1");
    expect_string(__wrap_wm_agent_upgrade_compare_versions, version2, WM_UPGRADE_NEW_UPGRADE_MECHANISM);
    will_return(__wrap_wm_agent_upgrade_compare_versions, -1);

    // wm_agent_upgrade_parse_task_module_request

    expect_value(__wrap_wm_agent_upgrade_parse_task_module_request, command, WM_UPGRADE_AGENT_UPDATE_STATUS);
    expect_value(__wrap_wm_agent_upgrade_parse_task_module_request, agent_id, agent_id);
    expect_string(__wrap_wm_agent_upgrade_parse_task_module_request, status, status);
    will_return(__wrap_wm_agent_upgrade_parse_task_module_request, task_request_status);

    // wm_agent_upgrade_task_module_callback

    expect_memory(__wrap_wm_agent_upgrade_task_module_callback, json, task_request_status, sizeof(task_request_status));
    will_return(__wrap_wm_agent_upgrade_task_module_callback, task_response_status);
    will_return(__wrap_wm_agent_upgrade_task_module_callback, 0);

    // wm_agent_upgrade_validate_task_status_message

    expect_memory(__wrap_wm_agent_upgrade_validate_task_status_message, input_json, task_response_status, sizeof(task_response_status));
    will_return(__wrap_wm_agent_upgrade_validate_task_status_message, agent_id);

    will_return(__wrap_queue_pop_ex_timedwait, OS_INVALID);
    expect_memory(__wrap_queue_pop_ex_timedwait, queue, upgrade_queue, sizeof(upgrade_queue));

    expect_value(__wrap_pthread_cond_signal, cond, &upgrade_threads_cond);

    wm_agent_upgrade_start_upgrade(args);

    assert_int_equal(upgrade_threads_count, 4);
}

void test_wm_agent_upgrade_start_upgrade_upgrade_custom_ok(void **state)
{
    (void) state;

    upgrade_threads_count = 5;

    int socket = 555;
    int agent_id = 25;
    char *status = "In progress";

    char *lock_restart = "025 com lock_restart -1";
    char *open_file = "025 com open wb test.wpk";
    char *write_file = "025 com write 5 test.wpk test\n";
    char *close_file = "025 com close test.wpk";
    char *calculate_sha1 = "025 com sha1 test.wpk";
    char *run_upgrade = "025 com upgrade test.wpk upgrade.sh";
    char *agent_res_ok = "ok ";
    char *agent_res_ok_0 = "ok 0";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a12c312ada12ab321a253a";

    test_upgrade_args *args = state[0];
    wm_manager_configs *config = args->config;
    wm_agent_task *agent_task = args->agent_task;
    wm_upgrade_custom_task *upgrade_custom_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = agent_id;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE_CUSTOM;
    upgrade_custom_task = wm_agent_upgrade_init_upgrade_custom_task();
    os_strdup("/tmp/test.wpk", upgrade_custom_task->custom_file_path);
    agent_task->task_info->task = upgrade_custom_task;

    // wm_agent_upgrade_send_wpk_to_agent

    will_return(__wrap_wm_agent_upgrade_validate_wpk_custom, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '025'");

    expect_string(__wrap_OS_SHA1_File, fname, "/tmp/test.wpk");
    expect_value(__wrap_OS_SHA1_File, mode, OS_BINARY);
    will_return(__wrap_OS_SHA1_File, "d321af65983fa412e3a12c312ada12ab321a253a");
    will_return(__wrap_OS_SHA1_File, 0);

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "/tmp/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_0);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_0) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a12c312ada12ab321a253a'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com upgrade test.wpk upgrade.sh'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 6);

    will_return(__wrap_queue_pop_ex_timedwait, OS_INVALID);
    expect_memory(__wrap_queue_pop_ex_timedwait, queue, upgrade_queue, sizeof(upgrade_queue));

    expect_value(__wrap_pthread_cond_signal, cond, &upgrade_threads_cond);

    wm_agent_upgrade_start_upgrade(args);

    assert_int_equal(upgrade_threads_count, 4);
}

void test_wm_agent_upgrade_start_upgrade_upgrade_err(void **state)
{
    (void) state;

    upgrade_threads_count = 5;

    int socket = 555;
    int agent_id = 25;
    char *status = "Failed";
    char *error = "Send lock restart error.";

    char *lock_restart = "025 com lock_restart -1";
    char *open_file = "025 com open wb test.wpk";
    char *write_file = "025 com write 5 test.wpk test\n";
    char *close_file = "025 com close test.wpk";
    char *calculate_sha1 = "025 com sha1 test.wpk";
    char *run_upgrade = "025 com upgrade test.wpk upgrade.sh";
    char *agent_res_err = "err ";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a12c312ada12ab321a253a";

    test_upgrade_args *args = state[0];
    wm_manager_configs *config = args->config;
    wm_agent_task *agent_task = args->agent_task;
    wm_upgrade_task *upgrade_task = NULL;

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = agent_id;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    cJSON *task_request_status = cJSON_CreateObject();

    cJSON_AddStringToObject(task_request_status, "module", "upgrade_module");
    cJSON_AddStringToObject(task_request_status, "command", "upgrade_update_status");
    cJSON_AddNumberToObject(task_request_status, "agent", agent_id);
    cJSON_AddStringToObject(task_request_status, "status", status);
    cJSON_AddStringToObject(task_request_status, "error_msg", error);

    cJSON *task_response_status = cJSON_CreateObject();

    cJSON_AddStringToObject(task_response_status, "error", WM_UPGRADE_SUCCESS);
    cJSON_AddStringToObject(task_response_status, "data", upgrade_error_codes[WM_UPGRADE_SUCCESS]);
    cJSON_AddNumberToObject(task_response_status, "agent", agent_id);
    cJSON_AddStringToObject(task_response_status, "status", status);

    // wm_agent_upgrade_send_wpk_to_agent

    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '025'");

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 2);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_err);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    // wm_agent_upgrade_parse_task_module_request

    expect_value(__wrap_wm_agent_upgrade_parse_task_module_request, command, WM_UPGRADE_AGENT_UPDATE_STATUS);
    expect_value(__wrap_wm_agent_upgrade_parse_task_module_request, agent_id, agent_id);
    expect_string(__wrap_wm_agent_upgrade_parse_task_module_request, status, status);
    expect_string(__wrap_wm_agent_upgrade_parse_task_module_request, error, error);
    will_return(__wrap_wm_agent_upgrade_parse_task_module_request, task_request_status);

    // wm_agent_upgrade_task_module_callback

    expect_memory(__wrap_wm_agent_upgrade_task_module_callback, json, task_request_status, sizeof(task_request_status));
    will_return(__wrap_wm_agent_upgrade_task_module_callback, task_response_status);
    will_return(__wrap_wm_agent_upgrade_task_module_callback, 0);

    // wm_agent_upgrade_validate_task_status_message

    expect_memory(__wrap_wm_agent_upgrade_validate_task_status_message, input_json, task_response_status, sizeof(task_response_status));
    will_return(__wrap_wm_agent_upgrade_validate_task_status_message, agent_id);

    will_return(__wrap_queue_pop_ex_timedwait, OS_INVALID);
    expect_memory(__wrap_queue_pop_ex_timedwait, queue, upgrade_queue, sizeof(upgrade_queue));

    expect_value(__wrap_pthread_cond_signal, cond, &upgrade_threads_cond);

    wm_agent_upgrade_start_upgrade(args);

    assert_int_equal(upgrade_threads_count, 4);
}

void test_wm_agent_upgrade_start_upgrade_upgrade_multiple(void **state)
{
    (void) state;

    upgrade_threads_count = 5;

    int socket = 555;
    int agent_id = 25;
    int agent_id_next = 35;
    char *status = "Failed";
    char *status_next = "In progress";
    char *error = "Send lock restart error.";

    char *lock_restart = "025 com lock_restart -1";
    char *lock_restart_next = "035 com lock_restart -1";
    char *open_file_next = "035 com open wb test.wpk";
    char *write_file_next = "035 com write 5 test.wpk test\n";
    char *close_file_next = "035 com close test.wpk";
    char *calculate_sha1_next = "035 com sha1 test.wpk";
    char *run_upgrade_next = "035 com upgrade test.wpk upgrade.sh";
    char *agent_res_ok = "ok ";
    char *agent_res_err = "err ";
    char *agent_res_ok_0 = "ok 0";
    char *agent_res_ok_sha1 = "ok d321af65983fa412e3a12c312ada12ab321a253a";

    test_upgrade_args *args = state[0];
    wm_manager_configs *config = args->config;
    wm_agent_task *agent_task = args->agent_task;
    wm_upgrade_task *upgrade_task = NULL;

    wm_agent_task *agent_task_next = NULL;
    wm_upgrade_task *upgrade_task_next = NULL;

    agent_task_next = wm_agent_upgrade_init_agent_task();
    agent_task_next->agent_info = wm_agent_upgrade_init_agent_info();
    agent_task_next->task_info = wm_agent_upgrade_init_task_info();

    config->chunk_size = 5;

    agent_task->agent_info->agent_id = agent_id;
    os_strdup("ubuntu", agent_task->agent_info->platform);
    agent_task->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task->wpk_sha1);
    agent_task->task_info->task = upgrade_task;

    agent_task_next->agent_info->agent_id = agent_id_next;
    os_strdup("ubuntu", agent_task_next->agent_info->platform);
    agent_task_next->task_info->command = WM_UPGRADE_UPGRADE;
    upgrade_task_next = wm_agent_upgrade_init_upgrade_task();
    os_strdup("test.wpk", upgrade_task_next->wpk_file);
    os_strdup("d321af65983fa412e3a12c312ada12ab321a253a", upgrade_task_next->wpk_sha1);
    agent_task_next->task_info->task = upgrade_task_next;

    queue_push(upgrade_queue, agent_task_next);

    cJSON *task_request_status = cJSON_CreateObject();

    cJSON_AddStringToObject(task_request_status, "module", "upgrade_module");
    cJSON_AddStringToObject(task_request_status, "command", "upgrade_update_status");
    cJSON_AddNumberToObject(task_request_status, "agent", agent_id);
    cJSON_AddStringToObject(task_request_status, "status", status);
    cJSON_AddStringToObject(task_request_status, "error_msg", error);

    cJSON *task_response_status = cJSON_CreateObject();

    cJSON_AddStringToObject(task_response_status, "error", WM_UPGRADE_SUCCESS);
    cJSON_AddStringToObject(task_response_status, "data", upgrade_error_codes[WM_UPGRADE_SUCCESS]);
    cJSON_AddNumberToObject(task_response_status, "agent", agent_id);
    cJSON_AddStringToObject(task_response_status, "status", status);

    // wm_agent_upgrade_send_wpk_to_agent

    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '025'");

    will_return(__wrap_isChroot, 0);

    expect_string(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK);
    expect_value(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM);
    expect_value(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR);
    will_return(__wrap_OS_ConnectUnixDomain, socket);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_err);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_err) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 2);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '025 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'err '");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_err);
    will_return(__wrap_wm_agent_upgrade_parse_agent_response, OS_INVALID);

    // wm_agent_upgrade_parse_task_module_request

    expect_value(__wrap_wm_agent_upgrade_parse_task_module_request, command, WM_UPGRADE_AGENT_UPDATE_STATUS);
    expect_value(__wrap_wm_agent_upgrade_parse_task_module_request, agent_id, agent_id);
    expect_string(__wrap_wm_agent_upgrade_parse_task_module_request, status, status);
    expect_string(__wrap_wm_agent_upgrade_parse_task_module_request, error, error);
    will_return(__wrap_wm_agent_upgrade_parse_task_module_request, task_request_status);

    // wm_agent_upgrade_task_module_callback

    expect_memory(__wrap_wm_agent_upgrade_task_module_callback, json, task_request_status, sizeof(task_request_status));
    will_return(__wrap_wm_agent_upgrade_task_module_callback, task_response_status);
    will_return(__wrap_wm_agent_upgrade_task_module_callback, 0);

    // wm_agent_upgrade_validate_task_status_message

    expect_memory(__wrap_wm_agent_upgrade_validate_task_status_message, input_json, task_response_status, sizeof(task_response_status));
    will_return(__wrap_wm_agent_upgrade_validate_task_status_message, agent_id);

    will_return(__wrap_queue_pop_ex_timedwait, 1);
    expect_memory(__wrap_queue_pop_ex_timedwait, queue, upgrade_queue, sizeof(upgrade_queue));

    // wm_agent_upgrade_send_wpk_to_agent

    will_return(__wrap_wm_agent_upgrade_validate_wpk, WM_UPGRADE_SUCCESS);

    expect_string(__wrap__mtdebug1, tag, "wazuh-modulesd:agent-upgrade");
    expect_string(__wrap__mtdebug1, formatted_msg, "(8162): Sending WPK to agent: '035'");

    will_return_count(__wrap_isChroot, 0, 6);

    expect_string_count(__wrap_OS_ConnectUnixDomain, path, DEFAULTDIR REMOTE_REQ_SOCK, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, type, SOCK_STREAM, 6);
    expect_value_count(__wrap_OS_ConnectUnixDomain, max_msg_size, OS_MAXSTR, 6);
    will_return_count(__wrap_OS_ConnectUnixDomain, socket, 6);

    // Lock restart

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(lock_restart_next));
    expect_string(__wrap_OS_SendSecureTCP, msg, lock_restart_next);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Open file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(open_file_next));
    expect_string(__wrap_OS_SendSecureTCP, msg, open_file_next);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Write file

    expect_string(__wrap_fopen, path, "var/upgrade/test.wpk");
    expect_string(__wrap_fopen, mode, "rb");
    will_return(__wrap_fopen, 1);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, config->chunk_size);

    will_return(__wrap_fread, "test\n");
    will_return(__wrap_fread, 0);

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(write_file_next));
    expect_string(__wrap_OS_SendSecureTCP, msg, write_file_next);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    expect_value(__wrap_fclose, _File, 1);
    will_return(__wrap_fclose, 0);

    // Close file

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(close_file_next));
    expect_string(__wrap_OS_SendSecureTCP, msg, close_file_next);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok) + 1);

    // Calculate file sha1

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(calculate_sha1_next));
    expect_string(__wrap_OS_SendSecureTCP, msg, calculate_sha1_next);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_sha1);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_sha1) + 1);

    // Run upgrade script

    expect_value(__wrap_OS_SendSecureTCP, sock, socket);
    expect_value(__wrap_OS_SendSecureTCP, size, strlen(run_upgrade_next));
    expect_string(__wrap_OS_SendSecureTCP, msg, run_upgrade_next);
    will_return(__wrap_OS_SendSecureTCP, 0);

    expect_value(__wrap_OS_RecvSecureTCP, sock, socket);
    expect_value(__wrap_OS_RecvSecureTCP, size, OS_MAXSTR);
    will_return(__wrap_OS_RecvSecureTCP, agent_res_ok_0);
    will_return(__wrap_OS_RecvSecureTCP, strlen(agent_res_ok_0) + 1);

    expect_string_count(__wrap__mtdebug2, tag, "wazuh-modulesd:agent-upgrade", 12);
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '035 com lock_restart -1'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '035 com open wb test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '035 com write 5 test.wpk test\n'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '035 com close test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok '");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '035 com sha1 test.wpk'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok d321af65983fa412e3a12c312ada12ab321a253a'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8165): Sending message to agent: '035 com upgrade test.wpk upgrade.sh'");
    expect_string(__wrap__mtdebug2, formatted_msg, "(8166): Receiving message from agent: 'ok 0'");

    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_sha1);
    expect_string(__wrap_wm_agent_upgrade_parse_agent_response, agent_response, agent_res_ok_0);
    will_return_count(__wrap_wm_agent_upgrade_parse_agent_response, 0, 6);

    will_return(__wrap_queue_pop_ex_timedwait, OS_INVALID);
    expect_memory(__wrap_queue_pop_ex_timedwait, queue, upgrade_queue, sizeof(upgrade_queue));

    expect_value(__wrap_pthread_cond_signal, cond, &upgrade_threads_cond);

    wm_agent_upgrade_start_upgrade(args);

    assert_int_equal(upgrade_threads_count, 4);
}

void test_wm_agent_upgrade_wait_available_thread(void **state) {
    int max_threads = 8;

    upgrade_threads_count = 5;

    wm_agent_upgrade_wait_available_thread(max_threads);

    assert_int_equal(upgrade_threads_count, 6);
}

void test_wm_agent_upgrade_wait_available_thread_max(void **state) {
    int max_threads = 8;

    upgrade_threads_count = 8;

    expect_value(__wrap_pthread_cond_wait, cond, &upgrade_threads_cond);
    expect_value(__wrap_pthread_cond_wait, mutex, &upgrade_threads_mutex);

    wm_agent_upgrade_wait_available_thread(max_threads);

    assert_int_equal(upgrade_threads_count, 9);
}

void test_wm_agent_upgrade_release_thread(void **state) {
    upgrade_threads_count = 5;

    expect_value(__wrap_pthread_cond_signal, cond, &upgrade_threads_cond);

    wm_agent_upgrade_release_thread();

    assert_int_equal(upgrade_threads_count, 4);
}

void test_wm_agent_upgrade_dispatch_upgrades(void **state) {
    wm_manager_configs *config = *state;

    upgrade_threads_count = 0;

    config->max_threads = 8;

    wm_agent_task *agent_task_next = NULL;
    wm_upgrade_task *upgrade_task_next = NULL;

    wm_agent_task *agent_task = wm_agent_upgrade_init_agent_task();

    config->chunk_size = 5;

    queue_push(upgrade_queue, agent_task);

    will_return(__wrap_queue_pop_ex, 1);
    expect_memory(__wrap_queue_pop_ex, queue, upgrade_queue, sizeof(upgrade_queue));

    expect_memory(__wrap_CreateThread, function_pointer, wm_agent_upgrade_start_upgrade, sizeof(wm_agent_upgrade_start_upgrade));
    expect_memory(__wrap_CreateThread, agent_task, agent_task, sizeof(agent_task));
    expect_memory(__wrap_CreateThread, config, config, sizeof(config));

    wm_agent_upgrade_dispatch_upgrades(config);

    assert_int_equal(upgrade_threads_count, 1);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        // wm_agent_upgrade_send_command_to_agent
        cmocka_unit_test_teardown(test_wm_agent_upgrade_send_command_to_agent_ok, teardown_string),
        cmocka_unit_test_teardown(test_wm_agent_upgrade_send_command_to_agent_recv_error, teardown_string),
        cmocka_unit_test_teardown(test_wm_agent_upgrade_send_command_to_agent_sockterr_error, teardown_string),
        cmocka_unit_test_teardown(test_wm_agent_upgrade_send_command_to_agent_connect_error, teardown_string),
        // wm_agent_upgrade_send_lock_restart
        cmocka_unit_test(test_wm_agent_upgrade_send_lock_restart_ok),
        cmocka_unit_test(test_wm_agent_upgrade_send_lock_restart_err),
        // wm_agent_upgrade_send_open
        cmocka_unit_test(test_wm_agent_upgrade_send_open_ok),
        cmocka_unit_test(test_wm_agent_upgrade_send_open_retry_ok),
        cmocka_unit_test(test_wm_agent_upgrade_send_open_retry_err),
        // wm_agent_upgrade_send_write
        cmocka_unit_test(test_wm_agent_upgrade_send_write_ok),
        cmocka_unit_test(test_wm_agent_upgrade_send_write_err),
        cmocka_unit_test(test_wm_agent_upgrade_send_write_open_err),
        // wm_agent_upgrade_send_close
        cmocka_unit_test(test_wm_agent_upgrade_send_close_ok),
        cmocka_unit_test(test_wm_agent_upgrade_send_close_err),
        // wm_agent_upgrade_send_sha1
        cmocka_unit_test(test_wm_agent_upgrade_send_sha1_ok),
        cmocka_unit_test(test_wm_agent_upgrade_send_sha1_err),
        cmocka_unit_test(test_wm_agent_upgrade_send_sha1_invalid_sha1),
        // wm_agent_upgrade_send_upgrade
        cmocka_unit_test(test_wm_agent_upgrade_send_upgrade_ok),
        cmocka_unit_test(test_wm_agent_upgrade_send_upgrade_err),
        cmocka_unit_test(test_wm_agent_upgrade_send_upgrade_script_err),
        // wm_agent_upgrade_send_wpk_to_agent
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_linux_ok, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_windows_ok, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_custom_custom_installer_ok, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_custom_default_installer_ok, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_run_upgrade_err, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_send_sha1_err, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_close_file_err, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_write_file_err, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_open_file_err, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_upgrade_lock_restart_err, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_validate_wpk_err, setup_config_agent_task, teardown_config_agent_task),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_send_wpk_to_agent_validate_wpk_custom_err, setup_config_agent_task, teardown_config_agent_task),
        // wm_agent_upgrade_start_upgrade
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_start_upgrade_upgrade_ok, setup_upgrade_args, teardown_upgrade_args),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_start_upgrade_upgrade_legacy_ok, setup_upgrade_args, teardown_upgrade_args),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_start_upgrade_upgrade_custom_ok, setup_upgrade_args, teardown_upgrade_args),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_start_upgrade_upgrade_err, setup_upgrade_args, teardown_upgrade_args),
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_start_upgrade_upgrade_multiple, setup_upgrade_args, teardown_upgrade_args),
        // wm_agent_upgrade_wait_available_thread
        cmocka_unit_test(test_wm_agent_upgrade_wait_available_thread),
        cmocka_unit_test(test_wm_agent_upgrade_wait_available_thread_max),
        // wm_agent_upgrade_release_thread
        cmocka_unit_test(test_wm_agent_upgrade_release_thread),
        // wm_agent_upgrade_dispatch_upgrades
        cmocka_unit_test_setup_teardown(test_wm_agent_upgrade_dispatch_upgrades, setup_config, teardown_config),
        // wm_agent_upgrade_prepare_upgrades

    };
    return cmocka_run_group_tests(tests, setup_group, teardown_group);
}