#ifndef CONFIG_TYPES_H
#define CONFIG_TYPES_H

#include <Arduino.h>

// 推送通道类型
enum PushType {
  PUSH_TYPE_NONE = 0,      // 未启用
  PUSH_TYPE_POST_JSON = 1, // POST JSON格式 {"sender":"xxx","message":"xxx","timestamp":"xxx"}
  PUSH_TYPE_BARK = 2,      // Bark格式 POST {"title":"xxx","body":"xxx"}
  PUSH_TYPE_GET = 3,       // GET请求，参数放URL中
  PUSH_TYPE_DINGTALK = 4,  // 钉钉机器人
  PUSH_TYPE_PUSHPLUS = 5,  // PushPlus
  PUSH_TYPE_SERVERCHAN = 6,// Server酱
  PUSH_TYPE_CUSTOM = 7,    // 自定义模板
  PUSH_TYPE_FEISHU = 8,    // 飞书机器人
  PUSH_TYPE_GOTIFY = 9,    // Gotify
  PUSH_TYPE_TELEGRAM = 10, // Telegram Bot
  PUSH_TYPE_WECOM = 11     // 企业微信群机器人
};

// 最大推送通道数
#define MAX_PUSH_CHANNELS 10

// WiFi 网络:第 1 个为主用,其余按序备用
#define WIFI_NETS_MAX 5
struct WifiNet {
  String ssid;
  String pass;
  // IP 记忆:同网络尽量复用首次获取的地址;网关/掩码变化时整体重学
  String ip;
  String gw;
  String mask;
  String dns;
};

// 自定义任务类型
enum CustomTaskType {
  TASK_NONE = 0,
  TASK_RESTART = 1,   // 定时重启设备
  TASK_AT = 2,        // 定时执行 AT 命令
  TASK_HTTP = 3       // 定时访问 HTTP URL(GET)
};
#define MAX_CUSTOM_TASKS 10
struct CustomTask {
  uint8_t type;            // CustomTaskType
  String name;             // 任务名称
  uint8_t mode;            // 0=间隔 1=每天 2=每周 3=每月
  uint32_t intervalSeconds;  // 间隔模式:执行间隔秒,0=停用
  uint8_t hour;            // 日历模式:时
  uint8_t minute;          // 日历模式:分
  uint8_t weekday;         // 每周模式:星期(0=周日)
  uint8_t dayOfMonth;      // 每月模式:几号(1-28)
  String param;            // AT命令或URL
  uint8_t httpMethod;      // HTTP任务:0=GET 1=POST
  String headers;          // HTTP任务:自定义头,每行 "Name: value"
  String body;             // HTTP任务:POST 请求体
};

// 推送通道配置（通用设计，支持多种推送方式）
struct PushChannel {
  bool enabled;           // 是否启用
  PushType type;          // 推送类型
  String name;            // 通道名称（用于显示）
  String url;             // 推送URL（webhook地址）
  String key1;            // 额外参数1（如：钉钉secret、pushplus token等）
  String key2;            // 额外参数2（备用）
  String customBody;      // 自定义请求体模板（使用 {sender} {message} {timestamp} 占位符）
};

// 配置参数结构体
struct Config {
  String smtpServer;
  int smtpPort;
  String smtpUser;
  String smtpPass;
  String smtpSendTo;
  String adminPhone;
  PushChannel pushChannels[MAX_PUSH_CHANNELS];  // 多推送通道
  String webUser;      // Web管理账号
  String webPass;      // Web管理密码
  String numberBlackList;  // 号码黑名单（换行符分隔）
  String wifiSsid;     // 兼容字段:仅用于旧键迁移
  String wifiPass;
  WifiNet wifiNets[WIFI_NETS_MAX];
  String brandTitle;      // 界面品牌主标题
  String brandSub;        // 界面品牌副标题
  CustomTask tasks[MAX_CUSTOM_TASKS];
  uint8_t pollSeconds;      // 网页状态刷新间隔秒,默认5
  String apiToken;          // API 访问令牌,空=关闭
};

// 默认Web管理账号密码
#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

// 长短信合并相关定义
#define MAX_CONCAT_PARTS 10       // 最大支持的长短信分段数
#define CONCAT_TIMEOUT_MS 30000   // 长短信等待超时时间(毫秒)
#define MAX_CONCAT_MESSAGES 5     // 最多同时缓存的长短信组数

// 长短信分段结构
struct SmsPart {
  bool valid;           // 该分段是否有效
  String text;          // 分段内容
};

// 长短信缓存结构
struct ConcatSms {
  bool inUse;                           // 是否正在使用
  int refNumber;                        // 参考号
  String sender;                        // 发送者
  String timestamp;                     // 时间戳（使用第一个收到的分段的时间戳）
  int totalParts;                       // 总分段数
  int receivedParts;                    // 已收到的分段数
  unsigned long firstPartTime;          // 收到第一个分段的时间
  SmsPart parts[MAX_CONCAT_PARTS];      // 各分段内容
};

#endif
