[TOC]

# 简介

基调听云3.0 CSDK是为C和C++程序员提供的开发包，通过SDK调用达到嵌码功能，辅助程序员分析程序中的问题。通过调用SDK的API埋点，度量分析事务的性能瓶颈和执行路径追踪，在线展示运行状态，协助开发者定位代码中的问题。因此，**该部署文档也是API的介绍和使用说明**。

CSDK的功能模块分为以下两部分:

- **进程内数据采集模块**：这部分以动态库形式提供，运行时动态加载， 负责将SDK调用产生的trace发送给汇总进程处理 。
- **事务数据汇总进程(守护进程)** ： 汇总进程作为守护进程常驻运行，ROOT模式安装时作为服务自动启动，需要安装。

# 支持列表

安装C/C++探针之前，请确保您的系统满足如下这些条件。

## 处理器架构

- x86_64（64位）
- x86（32位）

## 环境要求

| LIBC   | 版本         |
| ------ | ------------ |
| musl C | 1.1.20及以上 |
| glibc  | 2.6及以上    |

## 操作系统

| Linux发行版              | 版本                                                         |
| ------------------------ | ------------------------------------------------------------ |
| CentOS                   | 5.x/6.x/7.x/8.x                                              |
| Red Hat Enterprise Linux | 5.x/6.x/7.x/8.x                                              |
| Ubuntu                   | 14/15/16/17/18/19/20/22                                      |
| Debian                   | 7/8/9/10/11/12                                               |
| openSUSE                 | 12.1/12.2/12.3/13.1/13.2<br/>Leap:42.1/42.2/42.3/15.1/15.2<br/>Tumbleweed |
| Alpine                   | 3.9~3.18                                                     |


# 嵌码说明

## 概念
为了方便对程序问题的追踪，我们将应用程序执行过程分解成事务和组件两个概念。

- **事务**是一次服务请求的处理过程，例如：一次Web请求，或者RPC调用的server端处理过程。

- **组件**是事务处理过程中各个子功能的处理过程，例如: RPC外部调用、数据库调用、NoSQL调用、消息队列访问的生产者和消费者请求、功能/逻辑算法的计算过程、模块封装的过程。

## SDK API

由于C/C++应用程序框架/架构的多样性，**CSDK**提供两套SDK方法，分别用于异步方式调用的框架和同步方式调用的框架。

**头文件引入:**

```
#include "tingyun.h"
```

**初始化SDK:**

```
/**
 * 探针初始化函数。
 * 由于探针的初始化过程是异步方式在后台进行的，因此在探针初始化完成前的事务数据不会被采集。
 */
void TingYunAgentInit();
/**
 * 探针初始化函数。
 * 某些应用启动时会清除环境变量,这种场景下所有的参数配置都要通过配置文件设置,
 * 需要调用TingYunAgentConfigInit完成初始化。
 */
void TingYunAgentConfigInit(const char *config);//Agent初始化
```

**停止SDK:**

```
/*
 * 结束探针运行。
 */
void TingYunAgentStop();
```

### 异步API方法
#### 创建事务
**说明**：应用性能分解过程中，我们使用Action定义一个完整事务，通常它对应的是一个完整的http请求过程。

**代码**：

```
//args:
//  uri:    是Action对应的事务名，为空串则使用所在函数的类名::函数名。
//  return: 事务Id。
TActionId CreateAction(const char *uri);
//功能:     定义一个事务过程。
//调用时机: 客户端请求处理过程开始时。
```

#### 创建后台事务
**说明**：服务端应用中，可能会产生某些服务响应之外的事务处理过程，比如某些定时过程等。我们将其定义为后台任务。

**代码**：

```
//args:
//  cmd:   是后台事务对应的事务名，为空串则使用所在函数的类名::函数名。
//  reurn: 事务Id。
TActionId CreateBackGroundAction(const char *cmd);
//功能:     定义一个后台事务过程。
//调用时机: 后台事务开始时。
```
#### 结束事务
**说明**：事务结束时调用的方法，以次测量分析事务的执行过程 。

**代码**：

```
//args:
//  action:  事务id
//  return:  无
void ActionDestroy(TActionId *action);
//功能:     结束事务过程。
//调用时机: 事务处理过程结束时。
//说明:     ActionDestroy调用之后,即表示事物的数据采集过程完成,此后action和由action创建的所有component将失效。
```

### 创建Component
#### Component说明
一个事务通常会包含多个组件过程，组件过程还可能继续分解成几个其他组件过程。我们将这样的子过程定义为Component，通过对Component树的耗时分析来定位事务执行过程中的性能瓶颈。
#### 从Action创建Component
**一般组件**

```
//method 1:
//不指定函数名过程名，获取当前方法名作为组件名称。
//args:
//  action:  事务Id。
//  return:  组件Id。
TComponentId ACreateComponent(TActionId *action);
//功能:     开始一个组件的数据采集,这个API获取当前所处的函数名字作为组件名。
//调用时机: 一个可能耗时的计算类型的子过程开始的时候。
//
//method 2:
//自定义组件名称。
//args:
//  action:        事务Id。
//  ComponentName: 自定义组件名。
//  return:        组件Id。
TComponentId ActionCreateComponent(TActionId *action, const char *ComponentName);
//功能:     同上,这个API使用自定义的名字作为组件名。
//调用时机: 同上。
```

#### 从Component创建Component
```
//method 1:
//不指定函数名过程名，自动获取当前方法名作为组件名称。
//args: 
//  parent:  父级组件Id。
//  return:  组件Id。
TComponentId CCreateComponent(TComponentId *parent);
//功能:     开始一个组件的数据采集。
//调用时机: 当一个耗时的计算过程被分解为几个子过程时,我们需要采集每个子过程的耗时情况，来详细分析性能瓶颈。
//此时通过这个API可以构建一个调用树，通过分析每个过程的耗时情况，解决性能问题。
//
//method 2:
//自定义组件名称
//args:
//  parent:        父级组件Id。
//  ComponentName: 自定义组件名。
//  return:        组件Id。
TComponentId ComponentCreateComponent(TComponentId *parent, const char *ComponentName);
//功能:     同上, 差别是用此API自定义过程名。
//调用时机: 同上。
```

#### 数据库组件
```
//method 1:
//有SQL语句的情况下,使用SQL语句自动解析。
//args:
//  parentId: 事务Id或者过程组件Id。
//  type:     数据库类型: "Mysql"/"Postgresql"。
//  host:     数据库主机地址。
//  dbname:   数据库名。
//  sql:      执行的SQL语句。
//  return:   组件Id。
TComponentId CreateSQLComponent(TingYunID *parentId, const char *type, const char *host, const char *dbname, const char *sql);
//功能:     开始一个数据库调用过程的数据采集,需要传递SQL语句.内部将解析出数据库操作类型和表名，作为组件名的一部分。
//调用时机: 数据库过程开始时。
//
//method 2: 
//无SQL语句的情况下,传递表名和操作名(select/update/insert/delete)。
//args:
//  parentId: 事务Id或者过程组件Id 。
//  type:     数据库类型: "Mysql"/"Postgresql" 。
//  host:     数据库主机地址。
//  dbname:   数据库名。
//  table:    表名。
//  op:       表上的操作名。
//  return:   组件Id。
TComponentId CreateDBComponent(TingYunID *parentId, const char *type, const char *host, const char *dbname, const char *table, const char *op);
//功能:     同上,差别是明确指定表明和操作方法(select/update/insert/delete)。
//调用时机: 同上。
```

#### NoSQL组件
```
//args:
//  parentId:    事务Id或者过程组件Id。
//  type:        数据库类型: "Mysql"/"Postgresql"。
//  host:        数据库主机地址。
//  dbname:      数据库名。
//  object_name: 对象名。
//  op:          对象上的操作名。
//  return:      组件Id。
TComponentId CreateNoSQLComponent(TingYunID *parentId, const char *type, const char *host, const char *dbname, const char *object_name, const char *op);
//功能:     开始一个NoSQL数据库调用过程的数据采集。
//调用时机: 数据库过程开始时。
```

#### 外部调用组件(RPC、HTTP等)
```
//自定义组件名称。
//args:
//  parentId: 事务Id或者过程组件Id。
//  url:      外部调用的url。
//  return:   组件Id。
TComponentId CreateExternalComponent(TingYunID *parentId, const char *url);
//功能:     开始一个外部调用的数据采集,外部调用可以是向其他服务器发起的http请求，或者rpc调用。
//调用时机: 外部调用开始时
```

#### MQ调用组件(生产者、消费者)
```
//自定义组件名称。
//args:
//  parentId: 事务Id或者过程组件Id。
//  type:     MQ类型: RabbitMQ, ActiveMQ, Kafka...
//  host:     MQ地址。
//  queue:    队列名。
//  return:   组件Id。
TComponentId CreateConsumerComponent(TingYunID *parentId, const char *type, const char *host, const char *queue);
//功能:     开始一个消息队列消费者的数据采集。
//调用时机: MQ消费者开始时。
//
//自定义组件名称。
//args:
//  parentId: 事务Id或者过程组件Id。
//  type:     MQ类型: RabbitMQ, ActiveMQ, Kafka...
//  host:     MQ地址 。
//  queue:    队列名 。
//  return:   组件Id 。
TComponentId CreateProducerComponent(TingYunID *parentId, const char *type, const char *host, const char *queue);
//功能:     开始一个消息队列生产者的数据采集。
//调用时机: MQ生产者调用开始时。
```

#### 组件错误采集
```
//args:
//  component: 组件id。
//  message:   错误消息。
//  return:    无.
void ComponentSetError(TComponentId *component, const char *message);
//功能:     在组件过程中发生错误时,通过这个api记录代码行和调用栈,协助错误分析。
//调用时机: 对应组件的过程发生错误时。
```

#### 结束Component
```
//args:
//  component: 组件id。
//  return:    无.
void ComponentFinish(TComponentId *component);
//功能:     结束一个组件的数据采集。
//调用时机: 对应该组件的过程执行完毕时。比如数据库过程执行完时，或者一个外部调用执行完成时。
```

**子过程结束时，需要调用对应的Component.Finish()才能达到采集数据的目的。**

### 跨应用追踪
* 应用拓扑

  当一个账号下存在多个应用的相互调用关系时，可以利用API追踪应用之间的调用关系。
  调用者CreateTrackId，被调用者SetTrackId，在报表内就会产生“调用者” -> "被调用者" 的拓扑图。

客户端：
```
	//流程:
	//1.创建一个外部调用组件 。
	//2.调用ComponentCreateTrackId生成跨应用追踪id(字符串)。
	//3.发送id到server端 。
	//3.1如果外部调用为私有rpc协议，请自行传输这个id。
	//3.2如果是http(s)请求，将这个id作为http头的 "X-Tingyun-Id" 字段发送。
	//4.如果http应答头里携带”X-Tingyun-Tx-Data“，调用ComponentSetTxData完成跨应用追踪。
	//
	//需要的API:
	int ComponentCreateTrackId(TComponentId *component, char *idbuffer, int buffersize);
	void ComponentSetTxData(TComponentId *component, const char *tx_data);
	//调用过程:
	//1.
	TComponentId external == CreateExternalComponent(action, "http://192.168.1.253/servermethod");
	//2.
	char cross_id[1024] = {0};
	ComponentCreateTrackId(&external, cross_id, sizeof(cross_id) - 1);
	//添加cross_id到http请求头 (如果是http(s)) 。
	...
	//3.
	//发送数据到服务器。
	...
	//应答完成后，
	//取应答头里的 ”X-Tingyun-Tx-Data“ 。
	...
	//4.
	ComponentSetTxData(&external, tingyun_txdata);
	ComponentFinish(&external);
```
服务器端：
```
	void ActionSetTrackId(TActionId *action, const char *TrackId);
	char track_id[1024] = {0};
	//1.解析出客户端传过来的跨应用追踪ID。
	...
	//2.创建事务:
	TActionId action = CreateAction(appid, "/servermethod");
	//3.
	ActionSetTrackId(&action, track_id);
	//Action其他处理过程...
	...
	TingYunActionGetTxData(TActionId *action, char *buffer, int len);
	//buffer返回的数据我们称作tx-data
	//在服务端响应客户端的应答头(Response Header)里, 添加 "X-Tingyun-Tx-Data" 参数, 值为 tx-data 
	ActionDestroy(&action);
```
* 跨应用追踪
当产生拓扑关系的事务性能超过阈值时，会产生慢事务跟踪数据，同时在慢事务跟踪数据内会记录调用者和被调用者的详细追踪信息 。
通过点击慢事务跟踪图表内的链接，可以跳转到被调用者的详细追踪数据 。

## 注意事项:
### fork子进程,在子进程中调用API的
 **这种场景需要在子进程开始的地方再次调用 TingYunAgentInit()或TingYunAgentConfigInit 方法**

### 创建子组件的要求
 **只有一般组件(使用 ACreateComponent, ActionCreateComponent, CCreateComponent, ComponentCreateComponent 创建的组件) 才可以创建子组件.**

## API函数

+ **TActionId CreateAction(const char * uri);**

	```
	创建一个事务，URI可以是自定义的事务名。

	```
+ **void ActionAddCustomParam(TActionId * action, const char * k, const char * v);**

	```
	添加事务的自定义参数(URI解析参数)，协助应用在报表端分析慢过程追踪和错误追踪栈。
	适用情形: 需要通过参数分析慢事务或者错误原因时，请调用此接口采集参数。
	```
+ **void ActionAddRequestParam(TActionId * action, const char * k, const char * v);**

	```
	添加事务的客户端请求参数(HTTP头)，协助应用在报表端分析慢过程追踪和错误追踪栈。
	适用情形: 需要通过参数分析慢事务或者错误原因时，请调用此接口采集参数。
	```
+ **TComponentId ACreateComponent(TActionId * action);**

	```
	从Action创建一个一般组件，组件名使用当前调用方函数的名字。
	```
+ **TComponentId ActionCreateComponent(TActionId * action, const char * ComponentName);**

	```
	从Action创建一个一般组件，组件名使用自定义名字。
	```
+ **TComponentId CreateSQLComponent(TingYunID * parent_id, const char * type, const char * host, const char * dbname, const char * sql);**

	```
	从Action或一般组建之下创建一个数据库组件，参数为:数据库类型,主机名,库名,SQL语句。
	```
+ **TComponentId CreateDBComponent(TingYunID * parent_id, const char * type, const char * host, const char * dbname, const char * table, 
const char * op);**

	```
	从Action或一般组建之下创建一个数据库组件，参数为:数据库类型,主机名,库名,表名,操作名。
	```
+ **TComponentId CreateNoSQLComponent(TingYunID * parent_id, const char * type, const char * host, const char * dbname, const char * object_name, const char * op);**

	```
	创建NoSQL性能分解组件 参数:
	parent_id:   指向事务ID或组件ID
	type:        "mongo"/"memcache"/"redis"。
	host:        主机地址，可空。
	dbname:      库名称，可空。
	object_name: 对象名。
	op:          操作类型, ("GET", "SET" ...)。
	```
+ **TComponentId CreateExternalComponent(TingYunID * parent_id, const char * url);**

	```
	从Action或一般组建之下创建一个外部调用组件 参数: url:外部服务的url,格式: http(s)://host/uri, 例如 http://www.tingyun.com/。
	```
+ **void ActionIgnore(TActionId action);**

	```
	忽略这次Action的性能采集，只在ActionDestroy调用之前有效。
	适用情形: 通常不需要使用者调用。
	```
+ **void ActionSetName(TActionId * action, const char * classname, const char * methodname);**

	```
	修改事务名 参数: 类名,方法名。
	适用情形: 明确指定事物名。
	```
+ **void ActionSetStatus(TActionId * action, int StatusCode);**

	```
	设置Action对应的http事务应答的http状态码,缺省值200。
	适用情形: 能取到返回状态码的情况。
	```
+ **void ActionSetError(TActionId * action, const char * error_classname, const char * message);**

	```
	设置事务错误类名和消息。
	适用情形: 事务处理过程发生错误时调用。
	```
+ **void ActionSetTrackId(TActionId * action, const char * trackId)**

	```
	用于rpc调用或者http外部调用的跨应用追踪,trackId 由使用者从调用端传过来。
	适用情形: 参考跨应用追踪。
	```
+ **int TingYunActionGetTxData(TActionId * action, char * buffer, int len);**

	```
	用于rpc调用或者http外部调用的跨应用追踪,由被调用端调用,被调用端通过http响应头"X-Tingyun-Tx-Data" 或者其他方式(rpc)将数据传会调用端。
	适用情形: 参考跨应用追踪
	```
+ **void ActionSetUrl(TActionId * action, const char * Url);**

	```
	重设对应Action的 uri, 用于追踪慢过程和错误分析。
	适用情形: 明确指定事务对应的url,事务的命名优先使用url，没有的情况下使用函数名。
	```
+ **void ActionDestroy(TActionId * action)**

	```
	当前事务数据采集结束。
	适用情形: 事务结束。
	```
+ **TComponentId CCreateComponent(TComponentId * parent);**

	```
	对本组件再进行性能分解，创建下层性能分解组件，使用调用者所处函数作为组件名。
	```
+ **TComponentId ComponentCreateComponent(TComponentId * parent, const char * ComponentName);**

	```
	对本组件再进行性能分解，创建下层性能分解组件，使用ComponentName作为组件名。
	```
+ **int ComponentCreateTrackId(TComponentId * component, char * idbuffer, int buffersize);**

	```
	用于跨应用追踪,本组件内调用了外部应用过程或者发起了rpc调用,由应用此方法返回的结果携带到server端,server端通过ActionSetTrackId使用这个结果。最终在报表端生成跨应用追踪图表。
	适用情形: 参考跨应用追踪。
	```
+ **void ComponentSetTxData(TComponentId * component, const char * tx_data);**

	```
	用于跨应用追踪,如果外部调用的server端安装了听云的java,php等其他探针,启用跨应用追踪时，响应头里会有”X-Tingyun-Tx-Data“数据，通过本接口采集这个数据，完成跨应用追踪。
	适用情形: 参考跨应用追踪。
	```
+ **TActionId ComponentGetAction(TComponentId * component);**

	```
	取本组件所属的事务对象Action。
	适用情形: 某些情况下，传参过程同时携带事务ID和组件ID 会令代码变的复杂,这个API提供了方便方法通过组件获取到相应的事务id。
	```
+ **void ComponentFinish(TComponentId * component);**

	```
	停止性能分解组件计时 性能分解组件时长 = Finish时刻 - CreateComponent时刻 当时长超出堆栈阈值时，记录当前组件的代码堆栈。
	```
+ **void ComponentSetError(TComponentId * component, const char * message);**

	```
	设置组件错误消息。
	适用情形: 组件处理过程发生错误时调用。
	```
+ **int TingYunIDSerialize(const TActionId * action, char * out);**

	```
	在多进程协作事务处理框架中，事务的入口框架创建事务id，应用调用 TingYunIDSerialize 将事务id序列化成字符串，通过进程通信接口发送到协作进程。
	```
+ **TingYunID TingYunIDParse(const char * input);**

	```
	在多进程协作事务处理框架中，协作进程收到序列化后的字符串，解析出事务id。
	```

# 编译,链接

## 编译依赖
CSDK嵌码应用编译期依赖头文件 tingyun.h  
tingyun.h 的位置在 INSTALL_PATH/lib/tingyun3/csdk/include 路径下  

## 链接依赖
CSDK嵌码应用链接期依赖静态库文件libtingyun_sdk.a  
链接时需要将libtingyun_sdk.a文件放到链接器可以找到的路径下, 并且链接参数添加 -ltingyun_sdk  

# 嵌码示例

## Apache 模块开发 CSDK嵌码示例说明

以下用一个C开发的Apache模块 进行CSDK嵌码实例说明

### 1. Apache模块定义

From mod_helloworld.c
```C
/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA helloworld_module = {
    STANDARD20_MODULE_STUFF, 
    NULL,                      /* create per-dir    config structures */
    NULL,                      /* merge  per-dir    config structures */
    NULL,                      /* create per-server config structures */
    NULL,                      /* merge  per-server config structures */
    helloworld_directives,     /* table of config file commands       */
    helloworld_register_hooks  /* register hooks                      */
};
```
**说明**
+ helloworld_directives  
  模块定义的配置项初始化列表  

+ helloworld_register_hooks  
  模块注册各种回调函数的初始化入口函数  

### 2. Apache配置项初始化列表

From mod_helloworld.c
```C
/**
 * @brief Apache 配置项初始化列表
 * 
 * TingyunCSDKConfig: CSDK 配置文件路径
 * 
 * DataBaseFilePath:  项目数据库(sqlite3)文件存放路径
 * 
 */
static const command_rec helloworld_directives[] = {
    AP_INIT_TAKE1("TingyunCSDKConfig", set_config_path, NULL, RSRC_CONF, "TingyunAgent configuration file"),
    AP_INIT_TAKE1("DataBaseFilePath", set_db_path, NULL, RSRC_CONF, "Sqlite3 Database file"),
    { NULL }
};
```
**说明**
helloworld_directives 数组的每个元素注册一个Apache模块配置项的处理过程.  
本项目使用了名字为 "TingyunCSDKConfig" 和 "DataBaseFilePath" 的两个配置项.  
两个配置项分别在 set_config_path 和 set_db_path 两个回调函数中接收配置项的值,并做响应处理.  

### 3. 注册Apache回调函数

From mod_helloworld.c
```C
/**
 * @brief 注册回调函数
 * 
 * @param p 
 */
static void helloworld_register_hooks(apr_pool_t *p)
{
	/**
	 * @brief 注册子进程初始化回调函数
	 * 
	 */
	ap_hook_child_init(helloworld_child_init, NULL, NULL, APR_HOOK_MIDDLE);

	/**
	 * @brief 注册http请求的处理函数
	 * 
	 */
	ap_hook_handler(helloworld_handler, NULL, NULL, APR_HOOK_MIDDLE);
}
```
**说明**
+ ap_hook_child_init(helloworld_child_init, NULL, NULL, APR_HOOK_MIDDLE);  
  听云CSDK 需要在每个进程中初始化一次, 以初始化通信线程  
  Apache函数ap_hook_child_init函数提供了这个机会, 在helloworld_child_init函数内, 执行TingYunAgentConfigInit函数.  

+ ap_hook_handler(helloworld_handler, NULL, NULL, APR_HOOK_MIDDLE);  
  本项目实现了Http请求的处理过程.  
  通过ap_hook_handler函数,注册了http请求处理过程的回调函数.  

### 4. Apache模块Http入口函数

From mod_helloworld.c
```C
/**
 * @brief Http入口函数
 * 
 * @param r 
 * @return int 
 */
static int helloworld_handler(request_rec *r)
{
	if (strcmp(r->handler, "helloworld")) {
		return DECLINED;
	}
	const char *args = r->args;
	if ( args == NULL ) args = "";

	// http://host/helloworld?forward=http://otherhost/uri
	// 处理
	if ( strncmp(args, "forward=", 8) == 0 ) {
		return helloworld_forward_handler(r);
	}

	r->content_type = "text/html;charset=utf-8";

	// http://host/helloworld?init=xx
	// 数据库初始化
	if ( strncmp(args, "init=", 5) == 0 ) {
		database_init();
		ap_rprintf(r, "config=%s<br>\n", tingyun_config_file);
		return OK;
	}

	// 缺省处理过程
	if (!r->header_only) {

		const char *x_tingyun = apr_table_get(r->headers_in, "X-Tingyun");

		// 测试嵌码过程
		func1(x_tingyun);

		// 写配置文件路径
		ap_rprintf(r, "config=%s<br>\n", tingyun_config_file);

		// 写apache子进程PID
		ap_rprintf(r, "PID=%d<br><table><tr><th>Name</th><th>Value</th></tr>\n", getpid());

		const apr_array_header_t *headers_array = apr_table_elts(r->headers_in);
		apr_table_entry_t *headers = (apr_table_entry_t *)headers_array->elts;

		// http请求头写回页面
		for (int i = 0; i < headers_array->nelts; ++i) {
			const char *header_name = headers[i].key;
			const char *header_value = headers[i].val;
			ap_rprintf(r, "<tr><td>%s</td><td>%s</td></tr>\n", header_name, header_value);
		}

		// uri写回页面
		ap_rprintf(r, "<tr><td>uri</td><td>%s</td></tr>\n", r->uri);
		ap_rprintf(r, "<tr><td>unparsed_uri</td><td>%s</td></tr>\n", r->unparsed_uri);
		ap_rprintf(r, "<tr><td>args</td><td>%s</td></tr>\n", r->args);
		ap_rprintf(r, "</table>\n");
	}
	return OK;
}
```
**说明**
+ if (strcmp(r-&gt;handler, "helloworld")) ...  
  http的uri匹配判定  

+ if ( strncmp(args, "forward=", 8) == 0 ) ...  
  本模块实现的http转发请求, 转给helloworld_forward_handler函数处理  

+ if ( strncmp(args, "init=", 5) == 0 ) ...  
  数据库文件初始化接口  

+ if (!r-&gt;header_only) ...  
  本模块实现的缺省处理过程,  
  运行测试嵌码过程  
  将配置文件路径,pid,http头信息写回页面  

### 5. http请求转发函数
函数: helloworld_forward_handler  
本函数主要逻辑步骤分三部分:1,数据库记录转发请求;2,转发请求;3,数据库记录转发结果.  

1. 过滤请求方法  
  From forward_handler.h

	```C
		if ( r->method_number != M_GET && r->method_number != M_POST ) {
			r->status = 404;
			return 0;
		}
	```
    **说明**  
    只转发GET和POST请求  
2. 入口埋点(CSDK嵌码)  
  From forward_handler.h

	```C
		//埋点代码 开始
		TActionId action = CreateAction( "Forward" );
		ActionSetUrl( &action, r->unparsed_uri );
		const char *x_tingyun = apr_table_get(r->headers_in, "X-Tingyun");
		if ( x_tingyun ) {
			ActionSetTrackId(&action, x_tingyun);
		}
		SetTingYunThreadLocal(MakeTingYunIDStack(5)); //初始化线程局部存储(组件ID栈)
		TingYunStackPush(GetTingYunThreadLocal(), &action);
		//埋点代码 结束
	```
    **说明**
    + CreateAction  
     以指定的名字创建事务,返回事务的ID  
    + ActionSetUrl  
     关联事务对应的请求地址  
    + const char *x_tingyun = apr_table_get(r-&gt;headers_in, "X-Tingyun");  
     取APM跨应用追踪参数  
    + ActionSetTrackId(&action, x_tingyun);  
     关联跨应用追踪参数(如果有)  
    + SetTingYunThreadLocal(MakeTingYunIDStack(5));  
     初始化线程局部存储(组件ID栈),方便组件ID和事务ID的传递  
    + TingYunStackPush(GetTingYunThreadLocal(), &action);  
     事务ID入栈  
3. 数据库记录转发请求  
  From forward_handler.h

	```C
		unsigned long long current_time = utils_getsysusec();
		const char *forward_url = r->args + 8;
		unsigned long long id = xadd64(&increment_id, 1);
		int pid = getpid();
		const char *remote_addr = r->useragent_ip;
		//写数据库
		database_insert_request(forward_url, remote_addr, current_time, pid, id);
	```
4. 转发请求过程  
  From forward_handler.h

	```C
		int status = 0;
		char buffer_error[320] = {0};
		//转发请求
		int res = helloworld_forward(r, forward_url, &status, buffer_error, sizeof(buffer_error));
	```
5. 数据库记录转发结果  
  From forward_handler.h

	```C
		//写数据库
		if ( res == 0 && buffer_error[0] == 0 ) {
			database_insert_response(forward_url, remote_addr, status, current_time, pid, id);
		}
		else {
			database_insert_error(forward_url, remote_addr, status, buffer_error, current_time, pid, id);
		}
	```
6. 转发请求处理函数结束埋点  
  From forward_handler.h

	```C
		//埋点代码 开始
		ActionDestroy(&action);
		FreeTingYunbIDStack(SetTingYunThreadLocal(0));
		//埋点代码 结束
	```
    **说明**
    + ActionDestroy  
     结束事务性能数据采集  
    + FreeTingYunbIDStack(SetTingYunThreadLocal(0));  
     释放线程局部存储(组件ID栈)  

### 6. 转发请求逻辑及嵌码
函数: helloworld_forward  
本部分是5.4转发请求的实现细节逻辑及嵌码.  
1. 入口埋点(CSDK嵌码)  
  From forward_handler.h

	```C
		struct curl_slist *list = 0;

		//埋点代码 开始
		TComponentId sub     = CCreateComponent( TingYunStackTopItem( GetTingYunThreadLocal() ) );
		int place            = TingYunStackPush(GetTingYunThreadLocal(), &sub);
		TComponentId ext     = CreateExternalComponent( &sub, forward_url );
		char track_id[512]   = { 0 };
		strcpy(track_id, "X-Tingyun: ");
		ComponentCreateTrackId(&ext, &track_id[11], 500); //create track id;
		if ( track_id[0] ) {
			list = curl_slist_append(NULL, track_id);  //write track id into http request header.
		}
		//埋点代码 结束
	```
    **说明**
    + CCreateComponent( TingYunStackTopItem( GetTingYunThreadLocal() ) );  
     从栈上取父函数组件对应的ID, 创建当前函数对应的组件.  
    + TingYunStackPush(GetTingYunThreadLocal(), &sub);  
     当前函数组件ID入栈, 记录此前栈顶位置,待函数结束时恢复.  
    + CreateExternalComponent( &sub, forward_url );  
     以当前函数组件为父组件,创建外部调用子组件.  
    + ComponentCreateTrackId(&ext, &track_id[11], 500);  
     在此外部调用组件上,创建一个跨应用追踪ID,之后通过http头传递给后端(转发的目标服务)  
    + list = curl_slist_append(NULL, track_id);  
     写入跨应用追踪ID.  
2. 转发http头  
  From forward_handler.h

	```C
		//开始转发请求
		CURL *curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL, forward_url);
		//转发请求头
		const apr_array_header_t *headers_array = apr_table_elts(r->headers_in);
		apr_table_entry_t *headers = (apr_table_entry_t *)headers_array->elts;
		char buffer[1024];
		for (int i = 0; i < headers_array->nelts; ++i) {
			const char *header_name = headers[i].key;
			const char *header_value = headers[i].val;
			if ( strcmp(header_name, "Host") && strcmp(header_name, "Connection") && strcmp(header_name, "Cache-Control") ) {
				int size = snprintf(buffer, sizeof(buffer) - 1, "%s: %s", header_name, header_value);
				buffer[size] = '\0';
				list = curl_slist_append(list, buffer);
			}
		}
		if ( list ) {
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
		}
	```
3. 上下行数据转发和错误处理  
  From forward_handler.h

	```C
		HttpContext context = {
			.r = r,
			.upInited = 0,
			.initFine = 0,
			.externId = &ext,
		};
		// 添加http应答头的处理函数
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
		// 添加下载数据处理函数
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, recv_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);

		// 设置下载进度通知
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
		// curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
		// curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);

		// 设置连接超时
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4);
		// POST方法, 转发Post数据
		if ( r->method_number == M_POST ) {
			curl_easy_setopt(curl, CURLOPT_POST, 1);
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
			curl_easy_setopt(curl, CURLOPT_READDATA, &context);
		}
		// 开始curl处理
		CURLcode code = curl_easy_perform(curl);

		// 释放http头缓冲区
		if ( list ) curl_slist_free_all(list);

		// 转发错误处理
		if ( code != CURLE_OK ) {
			// 取错误描述信息
			const char *error_message = curl_easy_strerror(code);

			if ( buffer_error ) {
				strncpy(buffer_error, error_message, buffer_size - 1);
				buffer_error[buffer_size - 1] = '\0';
			}
			// 写500错误和错误信息
			r->status = HTTP_INTERNAL_SERVER_ERROR;
			ap_rprintf(r, "Internal Error:%s\n", error_message);
			//埋点代码,取ActionId,设置事务状态码
			ActionSetStatus(TingYunStackItem(GetTingYunThreadLocal(), 0), r->status);

			ap_finalize_request_protocol(r);
			code = OK;

			//埋点代码
			ComponentSetError(&ext, error_message);
		}

		long retcode = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &retcode);
		if ( retcode != 0 ) *status = (int)retcode;

		curl_easy_cleanup(curl);
	```
    **嵌码说明**
    + ActionSetStatus(TingYunStackItem(GetTingYunThreadLocal(), 0), r->status);  
     取栈上的第一个ID(事务ID): TingYunStackItem(GetTingYunThreadLocal(), 0)  
     调用ActionSetStatus设置事务状态码  
    + ComponentSetError(&ext, error_message);  
     设置组件的异常消息.  
4. 转发过程结束埋点  
  From forward_handler.h

	```C
		//埋点代码 开始
		ComponentFinish( &ext );
		ComponentFinish( &sub );
		//恢复栈
		TingYunStackRecover(GetTingYunThreadLocal(), place);
		//埋点代码 结束
	```
    **嵌码说明**
    + ComponentFinish( &ext );  
     外部调用结束.  
    + ComponentFinish( &sub );  
     当前函数结束.  
    + TingYunStackRecover(GetTingYunThreadLocal(), place);  
     恢复ID栈.  

### 7. 数据库记录转发请求逻辑及嵌码
函数: database_insert_request  
本部分是5.3数据库记录转发请求的实现细节逻辑及嵌码.  
1. 入口埋点(CSDK嵌码)  
  From database_sqlite.h

	```C
		//埋点代码 开始
		TComponentId *parent = TingYunStackTopItem(GetTingYunThreadLocal());
		TComponentId sub = CCreateComponent( parent );
		int place = TingYunStackPush(GetTingYunThreadLocal(), &sub);
		//埋点代码 结束
	```
    **嵌码说明**
    + TingYunStackTopItem(GetTingYunThreadLocal());  
     取父函数的组件ID  
    + CCreateComponent( parent );  
     创建当前函数对应的组件.  
    + TingYunStackPush(GetTingYunThreadLocal(), &sub);  
     当前组件ID入栈.  
2. 构建SQL语句,执行SQL语句  
  From database_sqlite.h

	```C
		char sql[1024];
		int size = snprintf(sql, sizeof(sql) - 1, "Insert into request (ID, UTCTIME, PID, REQID, CLIENTIP, URL) VALUES(NULL, %llu, %d, %llu, '%s', '%s');", start_time, pid, index, client_ip, fixed_url);
		if ( size > 0 ) sql[size] = '\0';

		int r = database_exec_sql(sql);
	```
3. 函数退出埋点  
  From database_sqlite.h

	```C
		//埋点代码 开始
		ComponentFinish(&sub);
		TingYunStackRecover(GetTingYunThreadLocal(), place);
		//埋点代码 结束
	```
    **嵌码说明**
    + ComponentFinish( &sub );  
     当前函数结束.  
    + TingYunStackRecover(GetTingYunThreadLocal(), place);  
     恢复ID栈.  

### 8. SQL语句执行函数及嵌码
函数: database_exec_sql  
本部分是7.2执行SQL语句的实现细节逻辑及嵌码.  
1. 入口埋点(CSDK嵌码)  
  From database_sqlite.h

	```C
		//埋点代码 开始
		TComponentId *parent = TingYunStackTopItem(GetTingYunThreadLocal());
		TComponentId dbcomponent = CreateSQLComponent(parent, "sqlite", database_filename, "", sql);
		//埋点代码 结束
	```
    **嵌码说明**
    + TingYunStackTopItem(GetTingYunThreadLocal());  
     取父函数的组件ID  
     调用ActionSetStatus设置事务状态码  
    + CreateSQLComponent( parent );  
     创建数据库组件.  
2. 执行SQL  
  From database_sqlite.h

	```C
		sqlite3 *db = NULL;
		char *zErrMsg = 0;
		int rc = 0;

		do {

			rc = sqlite3_open(database_filename, &db);
			if ( rc ) {

				//埋点代码
				ComponentSetError(&dbcomponent, "sqlite3_open failed");

				break;
			}
			rc = sqlite3_exec(db, sql, empty_callback, 0, &zErrMsg);
			if ( rc != SQLITE_OK ) {

				//埋点代码 开始
				const char *errorMsg = zErrMsg;
				if ( errorMsg == NULL ) errorMsg = "sqlite3_exec failed";
				ComponentSetError(&dbcomponent, errorMsg);
				//埋点代码 结束

				sqlite3_free(zErrMsg);
				zErrMsg = NULL;
			}

		}while ( 0 );

		if ( db ) sqlite3_close(db);
	```
    **嵌码说明**
    + ComponentSetError(&dbcomponent, "sqlite3_open failed");  
    + ComponentSetError(&dbcomponent, errorMsg);  
     SQL语句执行错误时, 调用ComponentSetError记录异常  
3. 数据库操作结束时嵌码  
  From database_sqlite.h

	```C
		//埋点代码
		ComponentFinish(&dbcomponent);
	```
    **嵌码说明**
    + ComponentFinish(&dbcomponent);  
     记录数据库操作结束时间.  


