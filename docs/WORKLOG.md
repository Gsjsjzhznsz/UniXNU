# Worklog — UniXNU-raphael-touchfix

## Task 36 (t-merge) — 合并 t 系列触摸驱动能力（配置下发 + 冷启动重试）（2026-11-08）

Agent: Claude (Gsjsjzhznsz session)

Task: UniXNU 内核（jackyhowh 0112）在本机触摸不可用；t 系列同协议驱动实测可用。
差异定位 = 0112 缺少 784 字节面板配置下发（无配置 IC 固件不上报触摸帧）与
t23/0102 冷启动重试阶梯。用户指示"合并 t 系列部分驱动"。

Work Log:
- 重放两条补丁链取得终态基线（apple-oss xnu-8796.141.3 + t 系列 0001-0102 /
  ios-sweet + 0101-0112）；两树 sweet_touch.c 分别 1218/975 行。
- 合并：新增 GT9886 命令通道 defines（0x6f68/0x6f78/0x80/0x82/0x83/0xff）、
  gt9886_write_reg（多字节写）、gt9886_cmd、gt9886_doze_disable（稳健版，
  替换错误恢复路径调用）、gt9886_send_config（五步 784B 下发）、
  gt9886_cfg.h（sensor0/1 双配置，原样自 t 系列）；touch_raphael_start 重写为
  3 次冷启动重试阶梯（200/50/10/500、300/100/10/500 + att2 INT strap）、
  PID"9886"+72B sum==0 双判据、sensor→配置选择（flip=3/0，sweettouchflip
  可覆盖）、T 格式判决行；轮询/板表/SweetHID 未动。
- 验证：新 0112 对 pre-0112 基线干净应用/反向应用/应用结果与交付件逐字节
  一致；gcc -fsyntax-only（stub 头）零警告。
- 构建路径：本 fork `touch-cfg-merge` 分支 → build-raphael CI（full）。

Stage Summary:
- 补丁 = patches/0112-raphael-touch-gt9886-normandy.patch（989 行，19 hunks，
  3 文件）。等待 CI 产物实机验证触摸。
