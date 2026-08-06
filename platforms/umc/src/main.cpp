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

#include "umcconsole.h"
#ifdef TINGYUN_ENABLED
#include <cstdlib>
#include "tingyun.h"
/* 包装函数：确保 TingYunAgentStop 在进程退出时先于其他全局清理运行。
 * std::atexit LIFO 顺序保证本函数在其他 atexit 注册的清理之前执行。 */
static void tingyun_atexit()
{
	TingYunAgentStop();
}
#endif

int main(int argc, const char * const *argv)
{
#ifdef TINGYUN_ENABLED
	/* TingYun APM agent initialization (async background startup).
	 * 通过 tingyun_atexit 包装函数注册，确保退出顺序。 */
	TingYunAgentInit();
	std::atexit(tingyun_atexit);
#endif

	UmcConsole console;
	console.Run(argc,argv);
	return 0;
}
