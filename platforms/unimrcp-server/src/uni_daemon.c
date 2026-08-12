/*
 * Copyright 2008-2015 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <apr_signal.h>
#include <apr_thread_proc.h>
#include "unimrcp_server.h"
#include "apt_log.h"
#ifdef TINGYUN_ENABLED
#include "tingyun.h"
#endif

static apt_bool_t daemon_running;

static void sigterm_handler(int signo)
{
	daemon_running = FALSE;
}

apt_bool_t uni_daemon_run(apt_dir_layout_t *dir_layout, apt_bool_t detach, apr_pool_t *pool)
{
	mrcp_server_t *server;

	daemon_running = TRUE;
	apr_signal(SIGTERM,sigterm_handler);

	apt_log(APT_LOG_MARK,APT_PRIO_NOTICE,"Run as Daemon");
	if(detach == TRUE) {
		apr_proc_detach(APR_PROC_DETACH_DAEMONIZE);
#ifdef TINGYUN_ENABLED
		/* fork 后子进程必须重新初始化听云 SDK（SDK 要求每个 fork 子进程
		 * 独立调用 TingYunAgentInit），父进程的 SDK 状态（PID、通信 socket
		 * 等）不适用于子进程。
		 * 注意：不在此处重新注册 atexit——main 中注册的 atexit 已被子进程继承，
		 * 在子进程退出时 LIFO 调用 TingYunAgentStop，覆盖本处重新初始化的实例。
		 * 若在此处再次 atexit 注册，子进程退出时会触发两次 TingYunAgentStop。 */
		TingYunAgentInit();
		apt_log(APT_LOG_MARK, APT_PRIO_NOTICE,
			"[TINGYUN] Agent re-initialized in daemon child after fork");
#endif
	}

	/* start server */
	server = unimrcp_server_start(dir_layout);
	if(!server) {
		return FALSE;
	}

	while(daemon_running) apr_sleep(1000000);

	/* shutdown server */
	unimrcp_server_shutdown(server);
	return TRUE;
}
