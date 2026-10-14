# ExamTimer 自动化测试脚手架

在目标 Windows 机器上跑了一套轻量代理，主机端用 Python 提供命令通道，
从而实现「远程执行命令 / 上传下载文件 / 抓取日志」的全自动真机测试。

## 组成

| 文件 | 位置 | 作用 |
| --- | --- | --- |
| `xp_server.py` | 主机 | 命令队列 + 文件收发 + 序列号匹配的 HTTP 服务（默认端口 8899） |
| `xpagent/agent.vbs` | 目标机 | 轮询代理：取命令、执行、回传输出；支持 `@DOWNLOAD` / `@UPLOAD` / `@START` / `@LOG` |
| `xpagent/runagent.bat` | 目标机 | 看门狗：代理退出或卡死时自动重启 |
| `xpagent/boot9.bat` | 目标机 | 引导脚本：清理旧文件并重新拉取代理 |
| `xpagent/runsuite.bat` | 目标机 | 10 个录像场景的一键回归 |
| `xp.sh` | 主机 | `xp.sh '<命令>' [超时]` 执行命令并等结果 |
| `xprun.sh` | 主机 | 往 RDP 桌面的「运行」对话框里敲命令（无代理时的兜底通道） |
| `xpput.sh` / `xpget.sh` | 主机 | 上传 / 下载文件 |

## 为什么这么设计（踩过的坑）

* **不用 RDP 驱动器重定向**：这台 XP 的 RDP 服务端不支持 `\\tsclient`（提示“找不到网络名”）。
* **不用 `Get-Content` 式管道读输出**：`wmic` 之类既不输出也不退出的命令会把管道读永久阻塞。
  改为把输出重定向到文件，再用完成标记文件 + 300 秒超时（超时杀进程树）。
* **`GET /cmd` 会被 WinINet 缓存**，导致同一条命令被反复执行 —— 改成 POST 轮询 + `no-store`。
* **批处理里调用批处理必须加 `call`**，否则外层批处理直接结束，完成标记永远写不进去。
* **XP 的 `tftp` 客户端不覆盖已存在的文件**，且创建的文件带只读属性 —— 引导脚本先
  `attrib -r -h -s` 再 `del /f`。
* **每条命令使用独立的临时文件名**，避免上一条命令遗留的文件被占用/只读而卡死下一条。

## 跑一遍

```bash
python3 test/xp_server.py &                 # 主机端服务
# 在目标机上通过 RDP 运行引导脚本（或直接双击）：
#   cmd /c tftp -i <主机IP> GET boot9.bat C:\xptest\boot9.bat & C:\xptest\boot9.bat
./test/xp.sh 'echo hello'                   # 验证通道
./test/xp.sh 'C:\xptest\runsuite.bat' 380   # 一键回归 10 个场景
./test/xp.sh '@UPLOAD C:\xptest\rt\t1.avi t1.avi'
```
