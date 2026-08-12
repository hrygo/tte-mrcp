#pragma once
#ifdef _WIN32 
#define __EXPORT_METHOD__ __declspec(dllexport)
#else
#define __EXPORT_METHOD__ __attribute__((visibility("default")))
#endif

#if defined(__GNUC__) || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) || (defined(__ICC) && (__ICC >= 600)) || defined(__ghs__)

# define __CURRENT_FUNCTION_NAME__ __PRETTY_FUNCTION__

#elif defined(__DMC__) && (__DMC__ >= 0x810)

# define __CURRENT_FUNCTION_NAME__ __PRETTY_FUNCTION__

#elif defined(__FUNCSIG__)

# define __CURRENT_FUNCTION_NAME__ __FUNCSIG__

#elif (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 600)) || (defined(__IBMCPP__) && (__IBMCPP__ >= 500))

# define __CURRENT_FUNCTION_NAME__ __CURRENT_FUNCTION_NAME__

#elif defined(__BORLANDC__) && (__BORLANDC__ >= 0x550)

# define __CURRENT_FUNCTION_NAME__ __FUNC__

#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)

# define __CURRENT_FUNCTION_NAME__ __func__

#elif defined(__cplusplus) && (__cplusplus >= 201103)

# define __CURRENT_FUNCTION_NAME__ __func__

#else

# define __CURRENT_FUNCTION_NAME__ "(unknown)"

#endif




#ifdef __cplusplus
extern "C" {
#endif
    typedef struct TingYunID {
        unsigned long long UniqueId;
        unsigned long long ActionId;
        unsigned long long startTime;
        void * actionData;
        unsigned int  ActionPID;
        unsigned char type;
        unsigned char methodSize;
        unsigned char trackIdSize;
        char data[160];
        //1.method_id
        //2.track_id
        //3.组件性能
    } TingYunID;
    typedef struct TingYunID TActionId;
    typedef struct TingYunID TComponentId;
#ifndef _TINGYUN_INSTANCE_SRC
    __EXPORT_METHOD__   void TingyunModuleInit();
    //参数要求: char *类型和 const char *类型一律使用UTF8编码
    //Agent级别API
    __EXPORT_METHOD__   void TingYunAgentInit();//Agent初始化
    __EXPORT_METHOD__   void TingYunAgentConfigInit(const char *config);//Agent初始化
    __EXPORT_METHOD__   void TingYunAgentStop();//Agent停止

    __EXPORT_METHOD__   TActionId TingYunAppCreateAction(const char * instance, const char *method, const char *filename, int line);//创建一个事务
    __EXPORT_METHOD__   TActionId TingYunAppCreateBackGroundAction(const char * instance, const char *method, const char *filename, int line);//创建一个事务

    //事务级别API
    __EXPORT_METHOD__   void TingYunActionAddCustomParam(TActionId *action_id, const char * k, const char * v);
    __EXPORT_METHOD__   void TingYunActionAddRequestParam(TActionId *action_id, const char * k, const char * v);
    __EXPORT_METHOD__   void TingYunActionIgnore(TActionId*);//忽略这个Action
    __EXPORT_METHOD__   void TingYunActionSetName(TActionId *action_id, const char * Instance, const char * Method);
    __EXPORT_METHOD__   void TingYunActionSetStatus(TActionId *action_id, unsigned short StatusCode, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   void TingYunActionSetError(TActionId *action_id, const char * classname, const char *message, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   void TingYunActionSetErrorTrace(TActionId *action_id, const char * tracedata, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   void TingYunActionSetStatusString(TActionId *action_id, const char * Status, const char * method, const char *filename, int line);//设置自定义状态
    __EXPORT_METHOD__   void TingYunActionSetTrackId(TActionId *action_id, const char * TrackId);
    __EXPORT_METHOD__   int  TingYunActionGetTxData(TActionId *action_id, char *buffer, int len);
    __EXPORT_METHOD__   void TingYunActionSetUrl(TActionId *action_id, const char * Url);
    __EXPORT_METHOD__   void TingYunActionDestroy(TActionId *action_id, const char *method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunActionCreate(TActionId *action_id, const char *method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunActionCreateComponent(TActionId *action_id, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunActionCreateExternalComponent(const TActionId *action_id, const char * url, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunActionCreateDBComponent(const TActionId *action_id, const char * type, const char * host, const char * dbname, const char * table, const char * op, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunActionCreateSQLComponent(const TActionId *action_id, const char * type, const char * host, const char * dbname, const char * sql, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunActionCreateNoSQLComponent(const TActionId *action_id, const char * type, const char * host, const char * dbname, const char * table, const char * op, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunActionCreateMQComponent(const TActionId *action_id, const char *type, int consumer, const char * host, const char * queue_name, const char * method, const char *filename, int line);


    //过程组件级别API
    __EXPORT_METHOD__   TComponentId TingYunComponentCreate(TComponentId *component_id, const char *method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunComponentCreateComponent(TComponentId *component_id, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunComponentCreateExternalComponent(TComponentId *component_id, const char * url, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunComponentCreateDBComponent(TComponentId *component_id, const char * type, const char * host, const char * dbname, const char * table, const char * op, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunComponentCreateSQLComponent(TComponentId *component_id, const char * type, const char * host, const char * dbname, const char * sql, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunComponentCreateNoSQLComponent(TComponentId *component_id, const char * type, const char * host, const char * dbname, const char * table, const char * op, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   TComponentId TingYunComponentCreateMQComponent(TComponentId *component_id, const char *type, int consumer, const char * host, const char * queue_name, const char * method, const char *filename, int line);
    __EXPORT_METHOD__   void TingYunComponentSetError(TComponentId *component_id, const char *message, const char *method, const char *filename, int line);
    __EXPORT_METHOD__   int TingYunComponentCreateTrackId(TComponentId*, char *, int);//创建一个跨应用追踪Id,返回id长度
    __EXPORT_METHOD__   void TingYunComponentFinish(TComponentId *component_id, const char *method, const char *filename, int line);
    __EXPORT_METHOD__   TActionId TingYunComponentGetAction(TComponentId*);//取创建这个Component(树)的Action
    __EXPORT_METHOD__   void TingYunComponentSetCrossInfo(TComponentId *component_id, const char * tx_name, const char * tx_id, int tx_duration);//外部调用返回时,如果获取到跨应用数据，传给ExternalComponent
    __EXPORT_METHOD__   void TingYunComponentSetSQL(TComponentId *component_id, char * sql);//追加一个慢Sql StackTrace
    __EXPORT_METHOD__   void TingYunComponentAppendStackTrace(TComponentId *component_id, char * trace);//追加一个慢Sql StackTrace
    __EXPORT_METHOD__   void TingYunComponentAppendSQLExplain(TComponentId *component_id, char * sql_explain);//追加一个sql执行计划(mysql/pg)结果
    __EXPORT_METHOD__   int TingYunComponentIsSlow(TComponentId *component_id); //判定是否慢sql;是->返回1,否->返回0
    __EXPORT_METHOD__   int TingYunComponentNeedStackTrace(TComponentId *component_id); //对SQL有效，判定是否需要采集StackTrace;是->返回1,否->返回0
    __EXPORT_METHOD__   int TingYunComponentNeedExplain(TComponentId *component_id); //对MySQL有效，判定是否需要使用执行计划分析;是->返回1,否->返回0
    __EXPORT_METHOD__   void TingYunComponentSetTxData(TComponentId *component_id, const char * tx_data);//将外部调用返回的 X-Tingyun-Tx-Data 添加到组件
    __EXPORT_METHOD__   TingYunID * TingYunIdThreadLocal();
    __EXPORT_METHOD__   int TingYunValidId(const TingYunID *id);

    __EXPORT_METHOD__ int TingYunIDSerialize(const TActionId *action_id, char *out);
    __EXPORT_METHOD__ TingYunID TingYunIDParse(const char *input);

    __EXPORT_METHOD__ TingYunID TingYunSyncActionEnter(const char *instance, const char *method, const char *filename, int line);
    __EXPORT_METHOD__ TingYunID TingYunSyncMethodEnter(const char *instance, const char *method, const char *filename, int line);
    __EXPORT_METHOD__ void      TingYunSyncMethodLeave(const TingYunID *id, const char *method, const char *filename, int line);
    __EXPORT_METHOD__ TingYunID TingYunSyncRPCEnter(const char *url, const char *method, const char *filename, int line);
    __EXPORT_METHOD__ TingYunID TingYunSyncNoSQLEnter(const char *dbtype, const char *host, const char *db, const char *table, const char *op, const char *method, const char *filename, int line);
    __EXPORT_METHOD__ TingYunID TingYunSyncDBEnter(const char *dbtype, const char *host, const char *db, const char *sql, const char *method, const char *filename, int line);
    __EXPORT_METHOD__ TingYunID TingYunSyncMQEnter(const char *mqtype, int consumer, const char * host, const char * queue_name, const char * method, const char *filename, int line);
    __EXPORT_METHOD__ void      TingYunSyncError(const char *message, const char *method, const char *filename, int line);


#endif //_TINGYUN_INSTANCE_SRC
#ifdef __cplusplus
}
#endif

#ifndef _TINGYUN_INSTANCE_SRC

//创建一个事务
#define CreateAction(instance)(TingYunAppCreateAction((instance), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define CreateBackGroundAction(instance)(TingYunAppCreateBackGroundAction((instance), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

//事务级别API
#define ActionAddCustomParam(action, key, value)(TingYunActionAddCustomParam((action), (key), (value)))
#define ActionAddRequestParam(action, key, value)(TingYunActionAddRequestParam((action), (key), (value)))

//忽略这个事务
#define ActionIgnore(action)(TingYunActionIgnore((action)))
#define ActionSetName(action, Instance, method)(TingYunActionSetName((action), (Instance), (method)))
#define ActionSetStatus(action, StatusCode)(TingYunActionSetStatus((action), (StatusCode), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define ActionSetStatusString(action, Status)(TingYunActionSetStatusString((action), (Status), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define ActionSetError(action, error_classname, message)(TingYunActionSetError((action), (error_classname), (message), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

#define ActionSetErrorTrace(action, tracedata, method)(TingYunActionSetErrorTrace((action), (tracedata), (method), __FILE__, __LINE__))
#define SetActionErrorTrace(action, tracedata)(TingYunActionSetErrorTrace((action), (tracedata), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

#define ActionSetTrackId(action, TrackId)(TingYunActionSetTrackId((action), (TrackId)))
#define ActionGetTxData(action)(TingYunActionGetTxData((action)))
#define ActionSetUrl(action, Url)(TingYunActionSetUrl((action), (Url)))

#define ActionDestroy(action)(TingYunActionDestroy((action), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

//type : action.(type) := TActionId; method.(type) := const char *
#define ActionCreateComponent(action, method)(TingYunActionCreateComponent((action), (method), __FILE__, __LINE__))
#define ACreateComponent(action)(TingYunActionCreateComponent((action), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
//type : action.(type) := TActionId;  url.(type) == method.(type) := const char *
#define CreateExternalComponent(action, url)(TingYunComponentCreateExternalComponent((action), (url), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

//type : tingyunID.(type) := TActionId | TComponentId;  type.(type) == host.(type) == dbname.(type) == table.(type) = op.(type) := const char *
#define CreateNoSQLComponent(tingyunID, type, host, dbname, table, op)(TingYunComponentCreateNoSQLComponent((tingyunID), (type), (host), (dbname), (table), (op), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define CreateDBComponent(tingyunID, type, host, dbname, table, op)(TingYunComponentCreateDBComponent((tingyunID), (type), (host), (dbname), (table), (op), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define CreateSQLComponent(tingyunID, type, host, dbname, sql)(TingYunComponentCreateSQLComponent((tingyunID), (type), (host), (dbname), (sql), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define CreateConsumerComponent(tingyunID, type, host, queue)(TingYunComponentCreateMQComponent((tingyunID), (type), 1, (host), (queue), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define CreateProducerComponent(tingyunID, type, host, queue)(TingYunComponentCreateMQComponent((tingyunID), (type), 0, (host), (queue), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

//过程组件级别API
//以下参数 component 类型 = TComponentId

//type : component.(type) := TComponentId; method.(type) := const char *
#define ComponentCreateComponent(component, method)(TingYunComponentCreateComponent((component), (method), __FILE__, __LINE__))
#define CCreateComponent(component)(TingYunComponentCreateComponent((component), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

#define ComponentSetError(component, message)(TingYunComponentSetError((component), (message), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

//component.(type) = TComponentId
#define ComponentFinish(component)(TingYunComponentFinish((component), __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))

//type : component.(type) := TComponentId;  out.(type) == := char *
#define ComponentCreateTrackId(component, out, size)(TingYunComponentCreateTrackId((component), (out), (size)))

//component.(type) = TComponentId
//返回: TActionId
#define ComponentGetAction(component)(TingYunComponentGetAction((component)))

#define ComponentSetTxData(component, tx_data)(TingYunComponentSetTxData((component),(tx_data)))
//type : component.(type) := TComponentId; tx_id.(type) := const char *; tx_duration.(type) := int(ms)
#define ComponentSetCrossInfo(component, tx_name, tx_id, tx_duration)(TingYunComponentSetCrossInfo((component_id),(tx_name), (tx_id), (tx_duration)))

#define C_SyncActionEnter(instance)(TingYunSyncActionEnter((instance),__CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define C_SyncTrackEnter(instance)(TingYunSyncMethodEnter((instance),__CURRENT_FUNCTION_NAME__, __FILE__, __LINE__))
#define C_SyncTrackLeave(id_pointer)(TingYunSyncMethodLeave((id_pointer), __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__))
#define C_SyncTrackRPCEnter(url)(TingYunSyncRPCEnter((url), __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__))
#define C_SyncTrackNoSQLEnter(dbtype, host, db, table, op)(TingYunSyncNoSQLEnter((dbtype), (host), (db), (table), (op), __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__))
#define C_SyncTrackDBEnter(dbtype, host, db, sql)(TingYunSyncDBEnter((dbtype), (host), (db), (sql), __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__))
#define C_SyncTrackMQEnter(dbtype, is_consumer, host, queue)(TingYunSyncMQEnter((dbtype), (is_consumer), (host), (queue), __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__))
#define C_SyncTrackConsumerEnter(dbtype, host, queue)(TingYunSyncMQEnter((dbtype), 1, (host), (queue), __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__))
#define C_SyncTrackProducerEnter(dbtype, host, queue)(TingYunSyncMQEnter((dbtype), 0, (host), (queue), __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__))
#define C_SyncTrackError(message)(TingYunSyncError((message), __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__))

#ifdef __cplusplus
class TingYunSyncAutoMethod 
{
    TingYunID current_id;
public:
    TingYunSyncAutoMethod(const char *inst, const char *method, const char *filename, int line)
    {
        current_id = TingYunSyncMethodEnter(inst, method, filename, line);
    }
    ~TingYunSyncAutoMethod()
    {
        TingYunSyncMethodLeave(&current_id, __CURRENT_FUNCTION_NAME__,__FILE__,__LINE__);
    }
};

#define _TINGYUN_DEFAULT_ __CURRENT_FUNCTION_NAME__, __FILE__, __LINE__

#define TINGYUN_VARIANT_DEFINE(text1,text2) __TingYun_Variant_##text2(text1,_TINGYUN_DEFAULT_)
#define TINGYUN_CONNECT_VALUE(text1,text2) TINGYUN_VARIANT_DEFINE(text1,text2)
#define TINGYUN_VALUE_NAME(text) TINGYUN_CONNECT_VALUE(text,__LINE__)
#define SyncTrackAction(text) TingYunSyncAutoMethod TINGYUN_VALUE_NAME(text)
#define SyncTrackMethod() SyncTrackAction("method")

#endif //__cplusplus

#endif


