# 呼叫中心 TTS 插件高并发杂音、静音、丢音问题排查与修复方案

## 1. 报告信息

| 项目 | 内容 |
| --- | --- |
| 排查对象 | 基于 UniMRCP 开发的 TTS 插件 |
| 核心源码 | `plugins/demo-synth/src/demo_synth_engine.c` |
| 排查日期 | 2026-08-03 |
| 问题表现 | 高并发场景偶发杂音、静音、丢音 |
| 证据口径 | 以当前工作区源码为唯一事实来源（SSOT），不采信文档、提交说明或旧二进制对实现状态的宣称 |
| 本次范围 | 静态源码分析、调用链分析、确定性 Socket 实验、编译验证；未修改插件源码，未使用生产日志或生产音频样本作结论 |

## 2. 结论摘要

当前问题不是单一的“缓冲区太小”，而是协议接收、实时音频读取和 PCM 分块处理三类缺陷叠加，并被全局任务串行阻塞、高频日志及会话内存累积放大。

已确认的两个最高优先级根因如下：

1. **WebSocket 被错误地当作消息边界明确的传输使用。** TCP 只保证有序字节流，不保证一次 `recv` 返回完整的 WebSocket 头或 HTTP 握手响应。当前代码多处假定一次接收即可获得完整结构，且没有保留 HTTP 握手之后已经读到的首个 WebSocket 帧，最终会导致帧解析错位、首帧丢失或接收线程提前退出。
2. **MPF 实时读线程在锁竞争时主动输出静音，而且同一次读取存在两次 `trylock`。** 即使环形缓冲区已有音频，第二次加锁也可能失败，插件随后输出一个完整静音帧。这可以直接解释高并发下的偶发静音。

同时确认：

- 奇数字节 PCM 分片会被直接丢弃最后一个字节，可能造成后续采样字节错位并形成杂音。
- WebSocket 分片消息未实现，`FIN` 位虽读取但未使用；合法的 continuation 帧会被当作未知帧处理。
- 客户端回复 Pong 和发送 Close 时未设置 MASK 位，不符合 WebSocket 客户端帧要求，严格服务端可能关闭连接并造成尾部丢音。
- 所有通道共享一个 UniMRCP consumer task，而连接、握手和初始请求在该任务中同步执行；一个慢连接可阻塞其他并发通道。
- 每次 SPEAK 从通道长生命周期内存池分配约 2.5 MiB 数据区，清理时仅置空指针，复用通道会持续增长内存并放大高并发抖动。

建议先完成 P0 协议流解码、PCM 字节连续性和实时读取互斥修复，再做异步连接、会话子池和日志治理。仅增加环形缓冲区容量不能解决上述根因。

## 3. 源码执行链

当前主要数据流如下：

```text
MRCP SPEAK
  -> demo_synth_channel_speak
     -> demo_synth_start_streaming
        -> DNS / TCP connect / websocket_handshake
        -> 发送 session.config、文本等 WebSocket 帧
        -> 启动 demo_synth_websocket_worker

WebSocket 接收线程
  -> websocket_recv_frame
  -> 识别 audio.start / 二进制 PCM / audio.end
  -> resample_pcm_to_8k
  -> pcm16_to_ulaw
  -> ring_buffer_write

UniMRCP MPF 实时线程
  -> demo_synth_stream_read_safe
  -> demo_synth_stream_read_audio
  -> 从 ring buffer 读取 PCMU
  -> 不足或锁竞争时输出 0xFF 静音
```

这个链路中至少有三种线程上下文：UniMRCP consumer task、WebSocket worker 和 MPF 实时读线程。问题集中在“字节流状态跨调用保存”和“跨线程共享状态的一致性”上。

### 3.1 源码证据索引

以下位置均指向排查时当前工作区的 `plugins/demo-synth/src/demo_synth_engine.c`，用于将结论锚定到源码实现：

| 位置 | 当前实现与证据 |
| --- | --- |
| 450 起 | `demo_synth_stream_read_audio` 内再次执行 `trylock`，不足整帧或锁竞争时返回 0 |
| 549 起 | `demo_tts_stream_thread` WebSocket worker；589 行从通道 pool 分配 2 MiB 接收缓冲区 |
| 870–880 | 奇数长度二进制 PCM 帧直接 `len--`，丢弃最后 1 字节 |
| 973 | 未处于音频态时忽略二进制数据 |
| 1100 起 | `demo_synth_start_streaming` 同步执行连接、握手和请求发送 |
| 1176–1178 | 每次 SPEAK 从通道 pool 分配 512 KiB 环形缓冲区 |
| 1254 | Socket 超时设置为 30 秒 |
| 1284 | 在 `speak_request` 发布之前读取它生成 `call_id` |
| 1369 | 引擎创建单个 `apt_consumer_task` |
| 2166 起、2220 | `demo_synth_channel_speak` 先启动流，之后才设置本次 `speak_request` |
| 2530 起、2907 | `demo_synth_stream_read_safe` 第一次 `trylock` 后解锁，再调用会二次 `trylock` 的读取函数 |
| 3586 起 | `websocket_handshake` 单次发送、单次接收且无 surplus 保存 |
| 3784 起 | `websocket_send_close` 发送未掩码客户端 Close |
| 3818 起、3832–3846 | `websocket_recv_frame` 一次读取基础帧头；读取 `FIN` 但未形成分片状态机 |
| 3929–3958 | Pong 手工组帧且未设置客户端 MASK 位 |

行号会随后续代码修改变化；函数名和行为描述是更稳定的复核入口。

## 4. 症状与根因映射

| 症状 | 直接触发路径 | 结论强度 |
| --- | --- | --- |
| 静音 | MPF 读线程 `trylock` 失败后主动填充 `0xFF`；或 `audio.start` 首帧被握手接收吞掉，后续二进制帧因未进入音频态而被忽略 | 已确认 |
| 丢音/截断 | WebSocket 头短读导致 worker 退出；未支持 continuation；客户端 Pong/Close 未掩码导致严格服务端断链；环形缓冲区写满丢弃 | 前三项已从源码确认，生产发生比例待日志验证 |
| 杂音 | HTTP/WS 边界数据被吞造成帧流错位；奇数字节 PCM 分片丢 1 字节后造成采样高低字节错位 | 已确认缺陷，具体生产占比待音频取证 |
| 首包慢/开始阶段静音 | 全局 consumer task 内同步 DNS、连接和握手，慢通道阻塞其他通道 | 已确认结构性放大因素 |
| 压测越久抖动越明显 | SPEAK 级大缓冲从通道池反复分配但不释放；逐帧 INFO 日志可能产生全局 I/O 竞争 | 已确认代码行为，运行时影响需量化 |

## 5. 根因一：WebSocket/TCP 接收状态机不完整（P0）

### 5.1 一次 `recv(2)` 不保证返回两个字节

`websocket_recv_frame` 在读取基础帧头时请求 2 字节，然后要求本次调用必须返回 2 字节：

```c
apr_size_t recv_len = 2;
rv = apr_socket_recv(sock, (char*)header, &recv_len);
if (rv != APR_SUCCESS || recv_len != 2) {
    return FALSE;
}
```

TCP 可以合法地只返回 1 字节。高并发、调度延迟、TLS/代理分段或网络拥塞都会提高短读出现概率。扩展长度和掩码字段存在同类一次读取假设。

已通过本地 `socketpair` 做确定性实验：发送端只写 1 字节，接收端执行 `recv(2)`，结果是成功返回 1 字节。按照当前判断逻辑，接收线程会将其视为异常并退出。

接收线程退出后，清理逻辑会把流标记为完成，MPF 侧只能读完已入环形缓冲区的数据，剩余语音被截断。因此这不是“理论上的网络异常”，而是错误处理合法 TCP 行为。

### 5.2 HTTP 握手只接收一次，并吞掉多读的 WebSocket 数据

`websocket_handshake` 只调用一次 `apr_socket_recv`，没有持续读取到 `\r\n\r\n`。如果服务端将 HTTP 101 响应和首个 WebSocket 帧合并在同一 TCP 包中，当前代码把整个缓冲区当作 HTTP 文本验证，然后直接丢弃头结束位置后的剩余字节。

已通过本地实验将 102 字节 HTTP 响应与 4 字节 WebSocket 帧一次发送；一次接收得到 106 字节，末尾确实包含首帧 `81 02 7b 7d`。当前实现没有 pending buffer，这 4 字节会被永久吞掉。

如果被吞掉的是 `audio.start`：

1. 插件未进入音频接收态；
2. 随后的二进制 PCM 帧进入“audio frame 之外”的分支；
3. PCM 被忽略；
4. 上游收到静音或空结果。

### 5.3 WebSocket 分片消息未实现

代码读取了 `FIN` 位，但没有使用。`opcode=0x0` 的 continuation 帧没有对应状态机，因此服务端只要对文本或二进制消息进行合法分片，插件就不能重组消息。

必须区分：

- TCP 分段：一个 WebSocket 帧可能跨多次 `recv`；
- WebSocket 分片：一个逻辑消息可能由首帧和多个 continuation 帧组成；
- 控制帧：Ping/Pong/Close 可以穿插在分片消息之间。

当前实现只覆盖“一个完整 WebSocket 帧恰好按预期边界到达”的理想路径。

### 5.4 客户端控制帧未掩码

当前 Pong 和 Close 帧使用 `MASK=0`。WebSocket 客户端发往服务端的帧必须掩码，包括控制帧。严格服务端在收到未掩码 Pong/Close 后可以关闭连接。

这类问题在长音频、连接保活或高并发下更容易暴露：服务端发送 Ping，插件回复非法 Pong，连接随后被关闭，表现为语音尾部丢失或截断。

### 5.5 握手发送也未处理短写

HTTP 请求发送只调用一次 `apr_socket_send`，若实际发送长度小于请求长度则直接失败。正确做法是持续发送直至全部完成或遇到不可恢复错误。

## 6. 根因二：实时读线程锁竞争时主动注入静音（P0）

`demo_synth_stream_read_safe` 首先对音频互斥锁执行一次 `apr_thread_mutex_trylock`：

- 成功：读取部分状态后立即解锁；
- 失败：设置 `emit_silence=TRUE`。

随后它调用 `demo_synth_stream_read_audio`，后者再次对同一把锁执行 `trylock`。这形成了明确的竞态窗口：

```text
MPF 线程：第一次 trylock 成功 -> 读取状态 -> unlock
WebSocket：获得锁 -> 正在写 ring buffer
MPF 线程：第二次 trylock 失败 -> 返回 0 字节 -> 输出整帧静音
```

也就是说，即使缓冲区中已经存在足够音频，只要第二次短暂竞争失败，本帧仍会被静音替代。并发越高、日志和调度抖动越大，竞争窗口越容易命中。

这里的 `0xFF` 对 PCMU 编码而言是静音值，本身不是杂音来源；问题是它在不应静音时被输出。

环形缓冲区数据并未立即消失，但音频时间轴被延迟。如果随后触发超时、STOP、连接清理或下一次 SPEAK，未及时消费的尾部数据就可能表现为丢音。

## 7. 根因三：奇数字节 PCM 分片被截断（P0）

当前二进制帧长度为奇数时，代码直接把长度减 1，丢弃末尾字节。16 位 PCM 的单个采样确实需要 2 字节，但网络分片边界不等于采样边界。

例如原始字节为：

```text
采样 A: A0 A1
采样 B: B0 B1
```

如果两个网络块恰好是：

```text
块 1: A0 A1 B0
块 2: B1 C0 C1 ...
```

当前代码丢弃 `B0`，下一块从 `B1` 开始按低字节解释，后续样本高低字节持续错位，可能形成明显杂音。正确做法是保留这个孤立字节，并与下一块首字节拼接。

现有 `pcm_accum` 只处理后续的 6 字节对齐，无法恢复已经丢弃的字节。

## 8. 高并发放大因素

### 8.1 单 consumer task 同步执行网络建连

所有通道共享一个 `apt_consumer_task`。`demo_synth_channel_speak` 在该任务内同步调用 `demo_synth_start_streaming`，后者执行：

- 地址解析；
- TCP connect；
- 最长 30 秒 Socket 超时；
- WebSocket HTTP 握手；
- 多个初始 WebSocket 消息发送；
- worker 创建。

一个慢 DNS、慢连接或慢握手就会阻塞后续通道的 SPEAK 处理。其主要表现是首包延迟和开始阶段静音，同时会让 MRCP 侧超时或 STOP 更容易与接收线程交错。

### 8.2 SPEAK 数据从通道池分配，复用时不释放

每次 SPEAK 至少分配：

- 约 512 KiB 环形缓冲区；
- 约 2 MiB WebSocket 接收缓冲区。

这些内存来自通道长生命周期 APR pool。清理时仅把指针设为 `NULL`，pool 内存不会归还。一个通道每执行一次 SPEAK，常驻池可能增加约 2.5 MiB，直到通道销毁。

持续压测时，这会带来 RSS 增长、缓存命中下降和更频繁的系统调度抖动，从而放大锁竞争与实时读线程欠载。

### 8.3 正常音频逐帧 INFO 日志

接收循环和正常音频帧路径存在 INFO 级逐帧日志。若生产 logger 允许输出这些日志，高并发下会产生明显的格式化、锁和磁盘/控制台 I/O 竞争。

日志通常不是协议错帧的根因，但会扩大 `trylock` 失败率、环形缓冲区欠载和首包延迟。

### 8.4 跨线程字段仅用 `volatile`，不构成同步

停止标志、Socket、请求指针、流状态和录音文件等字段被多个线程访问。`volatile` 不能提供原子性、互斥或线程间 happens-before 保证。清理、STOP 和 worker 退出交错时仍有数据竞争风险。

该项属于需治理的 P1 设计风险；在缺少生产崩溃栈和线程时序证据时，不把它宣称为本次杂音的首要根因。

### 8.5 `call_id` 取值时序错误

启动流时构造 `session.config.call_id`，读取的是 `synth_channel->speak_request`；但当前请求在 `demo_synth_start_streaming` 返回后才赋给该字段，而清理逻辑此前会将其置空。因此此处 `call_id` 实际为空。

是否影响后端关联需结合服务端实现验证，但源码层面的取值时序错误是确定的。

## 9. 次要安全与正确性问题

`resample_pcm_to_8k` 的调试日志固定读取前 10 个输入字节和前 5 个输出样本，但函数允许的最小有效输入可能小于该长度。日志表达式本身可能发生越界读取。应在打印前按实际长度限制循环范围，或删除这类逐样本日志。

HTTP 响应头名称按协议不区分大小写，而当前部分检查依赖固定大小写文本。合法但大小写不同的服务端响应可能被误判。建议使用正式的逐行头解析，不使用若干 `strstr` 作为握手状态机。

## 10. 为什么“加大缓冲区”不足以修复

加大环形缓冲区只能缓解生产速度与消费速度的短期差异，无法修复：

- TCP 短读；
- HTTP 响应与首个 WebSocket 帧粘连；
- WebSocket continuation；
- 客户端控制帧未掩码；
- 奇数字节 PCM 丢弃；
- 已有音频时因锁竞争主动输出静音；
- 全局 consumer task 被同步网络操作阻塞。

因此缓冲区调大只能作为容量调优，不能作为本问题的闭环方案。

## 11. 修复目标与必须保持的不变量

修复后应满足以下不变量：

1. Socket 每个成功接收的字节只消费一次，不丢失、不重复。
2. HTTP 头结束后的所有剩余字节必须进入 WebSocket 解码器。
3. WebSocket 解码器能够跨任意 TCP 分段恢复完整帧，并能重组 continuation 消息。
4. 任意 PCM 网络分块方式都必须得到与未分块输入完全一致的 PCM 字节流。
5. 环形缓冲区已有完整 MPF 帧时，读线程不能仅因短暂锁竞争输出静音。
6. 每次 SPEAK 的大块内存在会话结束后可回收。
7. 一个通道的慢连接不能阻塞其他通道进入 SPEAK。
8. 可通过计数器区分后端未产出、网络断流、环形缓冲区欠载、锁竞争和主动 STOP。

## 12. 修复方案与优先级

### 12.1 P0：实现有状态的 Socket/WebSocket 解码

需要统一解决以下问题：

- `send_all`：循环发送完整 HTTP/WS 数据；
- `recv_exact`：读取固定长度字段时循环到满足长度；
- 握手读取到 `\r\n\r\n`，只解析 HTTP 头；
- 将 HTTP 头后的 surplus 保存到连接级 pending buffer；
- 解码器优先消费 pending buffer，再读取 Socket；
- 保存当前帧头、扩展长度、掩码、payload 已收长度；
- 支持 continuation 和控制帧穿插；
- 客户端发送的所有帧统一执行掩码；
- 限制帧与消息最大长度，防止异常长度导致过量分配。

这不是一个局部一两行修复，需要调整连接上下文和 `websocket_recv_frame` 的函数签名。下面代码只展示可直接复用的基础 helper 骨架，不是完整补丁：

```c
static apt_bool_t socket_recv_exact(
    apr_socket_t *sock,
    unsigned char *buf,
    apr_size_t required)
{
    apr_size_t offset = 0;

    while (offset < required) {
        apr_size_t received = required - offset;
        apr_status_t rv = apr_socket_recv(
            sock,
            (char *)buf + offset,
            &received);

        if (received > 0) {
            offset += received;
        }

        if (rv != APR_SUCCESS) {
            if (APR_STATUS_IS_EINTR(rv)) {
                continue;
            }
            return FALSE;
        }

        if (received == 0) {
            return FALSE;
        }
    }

    return TRUE;
}

static apt_bool_t socket_send_all(
    apr_socket_t *sock,
    const unsigned char *buf,
    apr_size_t total)
{
    apr_size_t offset = 0;

    while (offset < total) {
        apr_size_t sent = total - offset;
        apr_status_t rv = apr_socket_send(
            sock,
            (const char *)buf + offset,
            &sent);

        if (sent > 0) {
            offset += sent;
        }

        if (rv != APR_SUCCESS) {
            if (APR_STATUS_IS_EINTR(rv)) {
                continue;
            }
            return FALSE;
        }

        if (sent == 0) {
            return FALSE;
        }
    }

    return TRUE;
}
```

连接上下文建议从裸 `apr_socket_t *` 扩展为：

```c
typedef struct websocket_connection_t {
    apr_socket_t *sock;

    /* 握手多读或上次解析剩余的字节。 */
    unsigned char *pending;
    apr_size_t pending_len;
    apr_size_t pending_capacity;

    /* WebSocket 分片消息状态。 */
    apr_byte_t fragmented_opcode;
    apt_bool_t fragmented_message_open;

    /* 防御性长度上限。 */
    apr_size_t max_frame_size;
    apr_size_t max_message_size;
} websocket_connection_t;
```

握手函数应返回“头部验证结果 + surplus”，而不是把整个首次接收缓冲区废弃：

```c
/* 伪代码：展示边界处理原则。 */
while (find_http_header_end(buffer, used) == NOT_FOUND) {
    receive_more_bytes(&buffer, &used);
}

header_end = find_http_header_end(buffer, used);
validate_http_101(buffer, header_end);

pending_len = used - header_end;
copy_to_connection_pending(buffer + header_end, pending_len);
```

客户端帧发送应统一走一个能够设置 MASK、生成随机掩码并异或 payload 的函数，Pong 和 Close 不再手写未掩码帧。

### 12.2 P0：保留跨帧 PCM 孤立字节

建议在每次 SPEAK 的音频上下文中增加 1 字节 carry。收到新块时先与 carry 拼接，然后只向后续重采样阶段提交偶数字节，最后一个孤立字节留给下一块。

```c
typedef struct pcm_stream_state_t {
    unsigned char carry_byte;
    apt_bool_t has_carry_byte;
} pcm_stream_state_t;

static void process_pcm_chunk(
    pcm_stream_state_t *state,
    const unsigned char *data,
    apr_size_t length)
{
    apr_size_t offset = 0;

    if (state->has_carry_byte && length > 0) {
        unsigned char sample[2];
        sample[0] = state->carry_byte;
        sample[1] = data[0];
        process_even_pcm_bytes(sample, sizeof(sample));
        state->has_carry_byte = FALSE;
        offset = 1;
    }

    if (length > offset) {
        apr_size_t even_length = (length - offset) & ~(apr_size_t)1;
        if (even_length > 0) {
            process_even_pcm_bytes(data + offset, even_length);
            offset += even_length;
        }
    }

    if (offset < length) {
        state->carry_byte = data[offset];
        state->has_carry_byte = TRUE;
    }
}
```

流正常结束时若仍有 carry，应记录协议/数据异常并丢弃这一个不完整采样；不能在每个网络块末尾静默丢弃。

### 12.3 P0：消除 MPF 读取的双重 `trylock`

短期修复是把“检查状态、计算可读长度、复制并推进 read_pos”合并到一次临界区，`demo_synth_stream_read_safe` 不再先解锁后调用一个会再次加锁的函数。

```c
static apr_size_t demo_synth_stream_try_read_audio(
    demo_synth_channel_t *channel,
    unsigned char *dst,
    apr_size_t requested,
    apt_bool_t *lock_contended)
{
    apr_size_t copied = 0;

    *lock_contended = FALSE;

    if (apr_thread_mutex_trylock(channel->audio_mutex) != APR_SUCCESS) {
        *lock_contended = TRUE;
        return 0;
    }

    /* 在同一次加锁中检查状态、读取 ring buffer、更新 read_pos/counters。 */
    copied = ring_buffer_read_locked(channel, dst, requested);

    apr_thread_mutex_unlock(channel->audio_mutex);
    return copied;
}
```

该短期修复消除了两次加锁之间的竞态，但实时线程仍可能因一次竞争而静音。最终建议将环形缓冲区改为单生产者/单消费者（SPSC）模型：

- WebSocket worker 是唯一生产者；
- MPF 是唯一消费者；
- 使用具备明确内存序的原子读写索引；
- 数据写完后发布 write index，数据读完后发布 read index；
- STOP/重置通过独立状态机和生命周期屏障处理；
- 不使用 `volatile` 代替原子与同步。

若项目当前 C 标准或 APR 原子能力不足，应保留一次短临界区方案，并对临界区时长和竞争次数做指标监控，不应未经验证自行实现弱内存序无锁结构。

### 12.4 P1：把建连和握手移出全局 consumer task

建议让 consumer task 仅处理 MRCP 状态迁移和任务投递，DNS、connect、握手、服务端请求全部由每个 SPEAK 的 worker 执行。worker 完成握手后再原子发布“可接收音频”状态。

必须定义清晰状态机：

```text
IDLE -> CONNECTING -> HANDSHAKING -> STREAMING -> DRAINING -> COMPLETED
                    \-> FAILED
任意活动态 --STOP--> STOPPING -> COMPLETED
```

每个状态只允许一个所有者执行 Socket 关闭和 SPEAK-COMPLETE，避免 STOP、错误回调和 worker 正常结束重复清理。

### 12.5 P1：为每次 SPEAK 创建可销毁的会话子池

在通道创建时保留 channel pool；每次 SPEAK 创建 `stream_pool`，把环形缓冲区、接收缓冲区、临时 JSON 和会话状态放入该子池。worker 完全退出并 join 后销毁子池。

```c
/* SPEAK 开始 */
apr_pool_create(&channel->stream_pool, channel->pool);

/* worker 已退出、Socket 已关闭、MPF 不再引用会话数据之后 */
if (channel->stream_pool) {
    apr_pool_destroy(channel->stream_pool);
    channel->stream_pool = NULL;
}
```

不能在线程仍可能访问内存时销毁 pool。互斥锁、条件变量和线程对象放在哪一层 pool，必须与 join/销毁顺序一起设计。

### 12.6 P1：修正请求字段发布时序

在启动流之前，把本次 SPEAK 请求或已复制的 call ID 写入会话上下文；不要在 `demo_synth_start_streaming` 返回后才发布。更稳妥的是只复制本次会话所需的不可变字段，避免 worker 长期引用 MRCP request 对象。

### 12.7 P2：日志与边界检查治理

- 正常音频逐帧日志降为 DEBUG/TRACE，或按会话每 N 帧采样；
- INFO 只保留状态变化、会话汇总和异常；
- 所有十六进制/样本预览按实际长度限制；
- 为帧长、累计消息长和 JSON 长度设置硬上限；
- 日志包含稳定 session ID，避免高并发下无法串联同一会话。

## 13. 建议修改范围

完整修复不是“小改动”。预计至少涉及：

- 通道/连接/会话结构体；
- `websocket_handshake`；
- `websocket_recv_frame` 或其替代状态机；
- WebSocket send/Pong/Close；
- WebSocket worker 的 PCM 处理；
- `demo_synth_stream_read_safe` 和 ring buffer 读写；
- `demo_synth_channel_speak`、STOP、cleanup 生命周期；
- 新增协议、PCM、并发和音频质量测试。

因此本报告提供的是关键代码骨架，不能把代码块直接视为完整可上线补丁。建议分两批提交：

1. P0 数据正确性：Socket/WS 解码、PCM carry、单次读锁；
2. P1 架构治理：异步建连、会话子池、原子状态机、日志指标。

## 14. 测试方案

### 14.1 Socket 与 WebSocket 确定性测试

使用 `socketpair` 或本地可控服务端，不依赖真实网络随机复现：

1. WebSocket 2 字节基础头按 `1+1` 字节发送；
2. 扩展长度按每个可能边界拆分；
3. payload 每次只发送 1 字节；
4. HTTP 101 与首个 WS 帧一次发送；
5. HTTP 头本身拆成多次发送；
6. 一个 TCP read 中包含多个完整 WS 帧；
7. 文本和二进制消息使用 continuation 分片；
8. continuation 中间插入 Ping；
9. 校验客户端 Pong、Close 和数据帧均已掩码；
10. 超大长度、非法 opcode、非法控制帧能够安全失败且不越界。

每个测试都应断言最终逻辑消息与原始输入逐字节相等，而不只断言函数返回成功。

### 14.2 PCM 分块等价性测试

准备固定 PCM16 输入，先计算“不分块处理”的基准输出；再遍历所有分块位置，包括大量 1 字节分块。所有分块方式得到的：

- 重组 PCM16；
- 8 kHz 重采样结果；
- PCMU 结果；
- 最终样本数

必须与基准完全一致。该测试可直接阻止“奇数字节被丢弃”回归。

### 14.3 环形缓冲区并发测试

以固定伪随机数据运行单生产者和单消费者：

- 生产者随机大小写入；
- 消费者按 MPF 固定帧长读取；
- 人为在索引发布前后插入 yield；
- 最终读取数据与输入完全一致；
- `written == read + dropped + remaining`；
- 缓冲区已有完整帧时不得计入 unexpected silence；
- STOP 和正常结束各跑一套时序。

若工具链支持，增加 ThreadSanitizer 构建，检查跨线程字段的数据竞争。

### 14.4 端到端并发压测

现有 `stress_test.py` 只等待固定 5 秒，并以日志出现 `SPEAK-COMPLETE` 作为成功条件，不能检测杂音、静音和丢音。应升级为音频内容校验。

建议在配置目标并发度下，使用可重复的固定 TTS 后端音频，至少采集：

- 请求数、成功数、超时数；
- SPEAK 到首个有效音频帧的延迟；
- 预期/实际音频样本数和时长误差；
- 原始 PCM/PCMU 字节数；
- ring written/read/dropped/remaining；
- 静音帧总数及原因分类；
- WebSocket 帧数、continuation 数、协议错误数；
- HTTP handshake surplus 字节数；
- 连接、握手、首包和整体耗时分位数；
- 进程 RSS 随同一通道重复 SPEAK 的增长曲线。

音频质量建议增加：

- 与基准音频的样本数或解码后波形对齐；
- 非预期连续静音区间；
- 相邻采样异常跳变峰值；
- RMS/能量异常；
- 首尾截断检测。

### 14.5 推荐回归矩阵

| 维度 | 建议覆盖 |
| --- | --- |
| 并发度 | 1、20、50、目标峰值、峰值以上安全余量 |
| 文本长度 | 短句、普通话术、长文本 |
| 音频长度 | 小于 1 秒、典型长度、长音频 |
| TCP 分段 | 正常、逐字节、随机分块、粘连多帧 |
| WebSocket | 单帧、continuation、Ping 穿插、服务端 Close |
| 控制流 | 正常完成、用户 STOP、连接超时、后端异常 |
| 通道复用 | 单次 SPEAK、同通道连续多次 SPEAK |

## 15. 验收标准

以下标准建议作为上线门禁，具体目标并发度和时延阈值由生产容量目标确定：

1. 所有确定性短读、粘包、continuation 测试逐字节通过。
2. 任意 PCM 分块方式的最终输出与基准完全一致。
3. 在目标并发压测中，WebSocket 协议解析错误为 0。
4. 无环形缓冲区覆盖丢弃：`ring_dropped == 0`。
5. 正常完成会话满足 `written == read`；若允许尾部不足一帧，需有明确且可核对的尾部处理规则。
6. 后端持续供给且 ring 中已有完整帧时，unexpected lock-contention silence 为 0。
7. 输出时长与预期样本数一致，不出现首部或尾部截断。
8. 同一通道重复 SPEAK 后 RSS 不随次数近似线性增长。
9. 慢连接只影响自身会话，不显著阻塞其他通道的 SPEAK 入队和建连。
10. STOP、网络失败和正常完成均只产生一次 SPEAK-COMPLETE，且无 use-after-free、重复关闭或线程遗留。

## 16. 可观测性改造

建议每个 SPEAK 结束时输出一条汇总日志，而不是逐帧 INFO：

```text
session_id=...
result=completed|stopped|network_error|protocol_error|timeout
connect_ms=...
handshake_ms=...
first_audio_ms=...
ws_frames=...
ws_continuations=...
http_surplus_bytes=...
pcm_input_bytes=...
pcmu_output_bytes=...
ring_written=...
ring_read=...
ring_dropped=...
silence_backend_wait=...
silence_buffer_empty=...
silence_lock_contention=...
duration_ms=...
```

其中静音原因必须分开统计，否则“后端尚未产出”和“插件锁竞争导致静音”会继续混在一个指标中。

## 17. 发布与回滚建议

1. 先在可控环境启用固定音频后端，完成字节级和音频级回归。
2. 小流量灰度 P0 修复，保留旧实现开关以便快速回滚。
3. 对比新旧版本的首包时延、静音原因、丢帧、协议错误和 RSS。
4. P0 稳定后再灰度异步建连和会话子池，避免一次发布同时改变协议与生命周期两条主线。
5. 灰度期间保留会话级原始输入/输出摘要；若合规允许，可对故障样本短期保留脱敏音频用于波形对比。
6. 回滚应以插件版本或特性开关为单位，不回滚用户其他工作区改动。

## 18. 现有验证证据与限制

本次已完成：

- 基于当前工作区源码追踪 SPEAK、WebSocket 接收、重采样、环形缓冲区和 MPF 读取链路；
- 通过本地 Socket 实验验证 TCP 短读，以及 HTTP 101 与首个 WebSocket 帧粘连会被一次接收同时读到；
- 使用当前可用 SDK/APR include 和 `x86_64` 目标完成插件源码编译，编译成功；
- 编译器提示 `is_fin`、`header_len` 等变量设置后未使用，与分片状态未实现的源码结论一致；
- 检查现有压测脚本，确认其只判断 `SPEAK-COMPLETE`，无法判定音频质量；
- 未发现覆盖 WebSocket 分段、PCM 分块和 ring 并发行为的现成自动化测试。

本次结论限制：

- 没有可用的当前生产故障日志、网络抓包或故障音频，因此不能给出各根因在生产中的发生占比；
- 旧运行日志只有启动/停止信息，不用于支撑本报告结论；
- 当前已安装插件二进制与工作区源码时间不一致，且用户要求源码作为 SSOT，因此本报告不从旧二进制反推现状；
- 当前工作区已有大量用户未提交修改，本次未修改或回退插件源码；排查阶段的编译验证重新生成了两个原本已处于修改状态的 `demo_synth_engine.o` 对象文件，报告阶段仅新增本文件。

## 19. 最终建议

按照以下顺序闭环：

1. **先修协议字节连续性**：完整读写、握手 surplus、WS continuation、客户端掩码。
2. **再修音频字节连续性**：奇数字节 carry，保证任意分块输出等价。
3. **消除实时读双重 trylock**：短期单次临界区，长期评估 SPSC。
4. **补齐确定性测试和音频内容校验**：不再把 `SPEAK-COMPLETE` 等同于音频正确。
5. **治理高并发架构**：建连异步化、每次 SPEAK 子池、明确生命周期状态机。
6. **以会话汇总指标灰度验证**：协议错误、静音原因、ring 守恒、音频时长和 RSS 同时达标后再扩大流量。

前三项直接针对杂音、静音和丢音；后续三项保证问题能被稳定复现、验证和长期防回归。
