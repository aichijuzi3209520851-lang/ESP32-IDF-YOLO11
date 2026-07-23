# DHT11 温湿度传感器问题分析与接线指南

> **文档日期**: 2026-07-21  
> **适用项目**: ESP32-S3 光伏板脏污识别智能监控系统  
> **ESP-IDF 版本**: v5.4（路径 `D:\ESP32-IDF\ESP32\v5.4\esp-idf`）  
> **问题描述**: DHT11 驱动代码已集成，编译烧录无报错，但网页端始终无法读取到温湿度数据（显示 `--.-°C` 和 `--% RH`）

---

## 一、问题现象

| 现象 | 状态 |
|------|------|
| `idf.py build` 编译 | ✅ 通过 |
| `idf.py flash` 烧录 | ✅ 成功 |
| 网页温度卡片 | ❌ 显示 `--.-°C` |
| 网页湿度卡片 | ❌ 显示 `--% RH` |
| 温湿度趋势图表 | ❌ 无数据点 |

---

## 二、软件数据链路验证

整条数据链路包含 5 个环节：

```
DHT11 传感器 ──GPIO──▶ dht11_task ──▶ sensor_data ──▶ /api/sensors ──▶ dashboard fetch
     ①硬件层           ②驱动层         ③共享层         ④HTTP 层         ⑤前端层
```

### 2.1 各环节代码审查

| 环节 | 源文件 | 审查结论 | 关键代码 |
|------|--------|----------|----------|
| ② 驱动层 | `main/dht11/dht11.c` | ✅ 正确 | `dht11_init()` 配置 OD + 上拉；`dht11_read()` 临界区保护微秒时序 |
| ② 任务层 | `main/main_app.cpp` L80-94 | ✅ 正确 | `dht11_task` 每 3s 调用 `dht11_read()`，成功→`sensor_data_update()`，失败→`sensor_data_invalidate()` |
| ③ 共享层 | `main/sensor_data/sensor_data.c` | ✅ 正确 | 互斥锁保护，`sensor_env_t.valid` 标记有效性 |
| ④ HTTP 层 | `main/http_stream/http_stream.c` L222-238 | ✅ 正确 | `/api/sensors` 端点返回 `{"temp":xx,"humi":xx,"ok":true/false}` |
| ⑤ 前端层 | `main/dashboard.html` L1343-1423 | ✅ 正确 | 每 3s `fetch('/api/sensors')`，根据 `ok` 字段显示值或占位符 |
| 编译配置 | `main/CMakeLists.txt` L28-34 | ✅ 正确 | `dht11/dht11.c` 和 `sensor_data/sensor_data.c` 均已注册编译 |

> **软件代码链路无遗漏。网页显示 `--.-` 意味着 API 返回了 `{"ok":false}`，即 `dht11_read()` 在运行时始终失败。**

### 2.2 网页端数据流逻辑（详细）

```javascript
// dashboard.html L1343-1351
function fetchSensorData() {
    fetch('/api/sensors', { cache: 'no-store' })
        .then(r => r.json())
        .then(d => { sensorData = d; })        // d.ok === true 时有数据
        .catch(() => { sensorData = { temp: null, humi: null, ok: false }; });
}

// dashboard.html L1387-1395  —— 展示逻辑
const envValid = sensorData.ok && Number.isFinite(sensorData.temp) && Number.isFinite(sensorData.humi);
dom.temp.textContent = envValid ? `${temp.toFixed(1)}°C` : '--.-°C';
dom.humidity.textContent = envValid ? `${humidity.toFixed(1)}% RH` : '--% RH';
```

显示 `--.-°C` 只有一个可能路径：`sensorData.ok === false`，即后端 `sensor_handler()` 返回了 `env.valid == false`。

---

## 三、GPIO38 引脚事实核查

### 3.1 之前的错误结论（已推翻）

> ~~❌ "GPIO38 是 SPI Flash 的片选引脚（SPICS0），在 Octal PSRAM 模式下被占用"~~

**这个结论是错误的。** 以下是通过 ESP-IDF v5.4 官方源码核实的事实：

### 3.2 ESP-IDF 源码验证（`spi_pins.h`）

文件路径：`D:\ESP32-IDF\ESP32\v5.4\esp-idf\components\soc\esp32s3\include\soc\spi_pins.h`

#### MSPI 引脚定义（Flash + PSRAM 共用的主 SPI 总线）

```c
// MSPI IOMUX PINs — 这些被 Flash/PSRAM 硬件占用
#define MSPI_IOMUX_PIN_NUM_CS1      26    // PSRAM 片选
#define MSPI_IOMUX_PIN_NUM_HD       27
#define MSPI_IOMUX_PIN_NUM_WP       28
#define MSPI_IOMUX_PIN_NUM_CS0      29    // ← Flash 片选是 GPIO29，不是 GPIO38！
#define MSPI_IOMUX_PIN_NUM_CLK      30
#define MSPI_IOMUX_PIN_NUM_MISO     31
#define MSPI_IOMUX_PIN_NUM_MOSI     32
#define MSPI_IOMUX_PIN_NUM_D4       33
#define MSPI_IOMUX_PIN_NUM_D5       34
#define MSPI_IOMUX_PIN_NUM_D6       35
#define MSPI_IOMUX_PIN_NUM_D7       36
#define MSPI_IOMUX_PIN_NUM_DQS      37
```

**MSPI 总线占用范围是 GPIO26~37。GPIO38 不在此列表中。**

#### SPI2 引脚定义（用户可用的通用 GPSPI）

```c
// SPI2 Octal 模式 IOMUX 映射（仅当用户使用 SPI2 Octal 时才占用）
#define SPI2_IOMUX_PIN_NUM_WP_OCT   38   // ← GPIO38 的 IOMUX Func2 功能
```

GPIO38 在 IOMUX Func2 下是 SPI2 的 WP 引脚，但这是**用户侧 GPSPI**，只有当代码主动初始化 SPI2 为 Octal 模式并使用 IOMUX 路由时才会占用。本项目没有初始化 SPI2，因此 **GPIO38 不被任何 SPI 外设占用**。

#### GPIO 有效性掩码

```c
// soc_caps.h
#define SOC_GPIO_VALID_GPIO_MASK    (0x1FFFFFFFFFFFFULL & ~(0ULL | BIT22 | BIT23 | BIT24 | BIT25))
```

GPIO38 在有效 GPIO 掩码中（仅排除了 GPIO22~25），**GPIO38 是合法的通用 IO 引脚**。

### 3.3 本项目 sdkconfig 确认

```
CONFIG_SPIRAM=y                     // 启用 PSRAM
CONFIG_SPIRAM_MODE_OCT=y            // Octal PSRAM 模式
CONFIG_SPIRAM_CLK_IO=30             // PSRAM CLK = GPIO30
CONFIG_SPIRAM_CS_IO=26              // PSRAM CS = GPIO26
CONFIG_ESPTOOLPY_FLASHMODE="dio"    // Flash 使用 DIO 模式
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y   // 16MB Flash（ESP32-S3-N16R8 模组）
```

**结论：在 Octal PSRAM 模式下，被占用的是 GPIO26~37（MSPI 总线）。GPIO38 是空闲可用的通用 GPIO。**

---

## 四、本项目完整 GPIO 引脚占用表

| GPIO | 用途 | 来源 | 能否用于 DHT11 |
|------|------|------|---------------|
| GPIO1 | ADC1_CH0（MAX471 电压采集） | `MAX471.c` | ❌ |
| GPIO2 | I2C SDA | `I2C.c` | ❌ |
| GPIO3 | I2C SCL | `I2C.c` | ❌ |
| GPIO4 | 摄像头 SCCB SDA | `camera.c` | ❌ |
| GPIO5 | 摄像头 SCCB SCL | `camera.c` | ❌ |
| GPIO6 | 摄像头 VSYNC | `camera.c` | ❌ |
| GPIO7 | 摄像头 HREF | `camera.c` | ❌ |
| GPIO8 | 摄像头 D2 | `camera.c` | ❌ |
| GPIO9 | 摄像头 D1 | `camera.c` | ❌ |
| GPIO10 | 摄像头 D3 | `camera.c` | ❌ |
| GPIO11 | 摄像头 D0 | `camera.c` | ❌ |
| GPIO12 | 摄像头 D4 | `camera.c` | ❌ |
| GPIO13 | 摄像头 PCLK | `camera.c` | ❌ |
| GPIO14 | LED 红灯（缺陷告警） | `led.h` | ❌ |
| GPIO15 | 摄像头 XCLK | `camera.c` | ❌ |
| GPIO16 | 摄像头 D7 | `camera.c` | ❌ |
| GPIO17 | 摄像头 D6 | `camera.c` | ❌ |
| GPIO18 | 摄像头 D5 | `camera.c` | ❌ |
| GPIO19 | LED 绿灯（正常指示） | `led.h` | ❌ |
| GPIO20 | USB D+（如不使用 USB 可用） | — | ⚠️ |
| GPIO21 | 空闲 | — | ✅ |
| GPIO22~25 | 无效引脚（ESP32-S3 不可用） | `soc_caps.h` | ❌ |
| GPIO26~37 | MSPI 总线（Flash + PSRAM） | `spi_pins.h` | ❌ |
| **GPIO38** | **当前 DHT11 DATA（代码定义）** | `main_app.cpp` L28 | **✅ 可用** |
| GPIO39~42 | 空闲（JTAG 默认引脚，不调试时可用） | — | ✅ |
| GPIO43 | UART0 TXD（串口日志输出） | — | ⚠️ 慎用 |
| GPIO44 | UART0 RXD（串口日志输入） | — | ⚠️ 慎用 |
| GPIO45~46 | Bootstrap Strapping 引脚 | — | ⚠️ |
| GPIO47 | 空闲 | — | ✅ |
| GPIO48 | 板载 LED（Blink） | `sdkconfig` CONFIG_BLINK_GPIO | ❌ |

> **结论：GPIO38 本身在软件层面完全可用于 DHT11。问题不在 GPIO 编号选择上。**

---

## 五、系统化故障排查清单

既然 GPIO38 可用，代码链路完整，问题必然出在 **以下某一层或多层**。请按顺序逐项排查：

### 第 1 层：串口日志确认（最优先）

**操作**：烧录后打开串口监视器 `idf.py monitor`，观察 DHT11 相关日志。

| 观察到的日志 | 含义 | 下一步 |
|-------------|------|--------|
| `I (xxx) CAM_APP: DHT11: 25.3C 62.0%` | ✅ DHT11 读取成功 | 跳到第 3 层排查 HTTP/前端 |
| `W (xxx) DHT11: Sensor response timeout` | ❌ 传感器无应答 | 排查第 2 层（硬件接线） |
| `W (xxx) DHT11: Data timeout at bit N` | ❌ 数据传输中断 | 排查第 2 层（信号质量/上拉电阻） |
| `W (xxx) DHT11: Checksum mismatch` | ⚠️ 数据损坏 | 排查接线长度、去耦电容、上拉电阻 |
| 完全没有 DHT11 相关日志 | ❌ 任务可能未启动 | 排查第 2.5 层（任务创建问题） |

### 第 2 层：硬件接线排查

#### 2.1 DHT11 模块类型确认

DHT11 有两种常见封装，接法不同：

**4 针裸传感器（蓝色塑封体）**

```
         ┌───────────────────┐
         │      DHT11        │
         │    (正面网格朝你)   │
         │  ┌──┬──┬──┬──┐   │
         │  │1 │2 │3 │4 │   │
         └──┴──┴──┴──┴──┴───┘
            │  │  │  │
           VCC DATA NC GND
```

| 引脚 | 功能 | 接法 |
|------|------|------|
| Pin 1 | VCC | 接 ESP32-S3 **3.3V** |
| Pin 2 | DATA | 接 ESP32-S3 **GPIO38**，并通过 **4.7KΩ~10KΩ 电阻上拉到 3.3V** |
| Pin 3 | NC | **不接** |
| Pin 4 | GND | 接 ESP32-S3 **GND** |

> ⚠️ **4 针裸传感器必须外接上拉电阻，否则数据线电平不稳，无法通信！**

**3 针模块板（蓝色 PCB 小板，板载上拉电阻）**

```
    ┌─────────────────────┐
    │  ┌──────────────┐   │
    │  │    DHT11     │   │
    │  └──────────────┘   │
    │    R（板载上拉）      │
    │  ┌──┬──┬──┐         │
    │  │S │V │G │         │
    └──┴──┴──┴──┴─────────┘
       │  │  │
      DATA VCC GND
```

| 引脚 | 标注 | 接法 |
|------|------|------|
| S / OUT / DATA | 数据 | 接 GPIO38 |
| V / VCC / + | 电源 | 接 3.3V |
| G / GND / - | 地 | 接 GND |

> 3 针模块板已集成上拉电阻，**无需额外加电阻**。

#### 2.2 接线验证清单

- [ ] DATA 线是否确实连接到了 **GPIO38** 引脚？（而不是开发板上标注为其他编号的排针）
- [ ] VCC 接的是 **3.3V** 还是 5V？（推荐 3.3V，使用 5V 时 DATA 高电平可能超过 ESP32-S3 耐压）
- [ ] GND 是否与 ESP32-S3 共地？
- [ ] 如果是 4 针裸传感器，DATA 和 VCC 之间是否有 **4.7KΩ~10KΩ 上拉电阻**？
- [ ] 杜邦线/跳线是否接触良好？有无松动或虚接？
- [ ] 线材长度是否过长？（建议控制在 20cm 以内）
- [ ] DHT11 模块是否有损坏？（可以尝试更换一个传感器模块测试）

#### 2.3 开发板引脚标注 vs GPIO 编号

> ⚠️ **很多 ESP32-S3 开发板的丝印标注和 ESP32-S3 芯片的实际 GPIO 编号不一致！**

不同厂家的 ESP32-S3-CAM 开发板，PCB 上印的引脚编号可能是板载编号而非 GPIO 编号。**请务必对照你的开发板原理图或引脚图确认 GPIO38 对应的物理排针位置。**

### 第 2.5 层：任务调度与资源竞争

注意当前项目在 Core 1 上运行的任务：

| 任务名 | Core | 优先级 | 功能 |
|--------|------|--------|------|
| `dht11` | **Core 1** | 5 | DHT11 每 3s 读取 |
| `inference` | **Core 1** | 5 | YOLO 推理（CPU 密集，单次 4~5 秒） |

`dht11_task` 和 `inference_task` 都绑定在 Core 1，优先级相同。虽然 DHT11 驱动在数据交换阶段使用了 `portENTER_CRITICAL()` 保护（禁用了 Core 1 的任务调度），但需要注意：

- `dht11_read()` 第 59-60 行的**起始信号**（拉低 20ms）发生在临界区**之外**
- 如果推理任务恰好在此时抢占了 CPU，起始信号的时序可能被打断

> 这通常不会导致 100% 失败，但如果推理任务占用率极高，可能增加失败率。

**验证方法**：临时将 DHT11 任务改到 Core 0 测试：
```c
// main_app.cpp L160，将最后一个参数从 1 改为 0
xTaskCreatePinnedToCore(dht11_task, "dht11", 3072, NULL, 5, NULL, 0);
```

### 第 3 层：HTTP API 层验证

**操作**：在浏览器中直接访问 `http://<ESP32-IP>/api/sensors`

| 返回内容 | 含义 |
|---------|------|
| `{"temp":25.3,"humi":62.0,"ok":true}` | ✅ 传感器数据正常到达 HTTP 层 |
| `{"temp":null,"humi":null,"ok":false}` | ❌ sensor_data 模块中数据无效 |
| 无法访问 / 连接超时 | ❌ HTTP 服务器或 WiFi 问题 |

### 第 4 层：前端层验证

**操作**：在浏览器开发者工具（F12）→ Console 中运行：

```javascript
fetch('/api/sensors').then(r => r.json()).then(d => console.log(d));
```

确认返回的 JSON 中 `ok` 字段值。如果后端返回 `ok:true` 但页面仍显示 `--.-`，则可能是前端缓存或 JavaScript 异常。

---

## 六、DHT11 技术参数与通信协议

### 6.1 基本参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 工作电压 | 3.3V ~ 5.5V | ESP32-S3 推荐使用 3.3V |
| 温度范围 | 0°C ~ 50°C | 精度 ±2°C |
| 湿度范围 | 20% ~ 90% RH | 精度 ±5% RH |
| 最小采样间隔 | ≥ 1 秒 | 当前代码设 3 秒，满足要求 |
| 数据位数 | 40 bit | 8bit 湿度整数 + 8bit 湿度小数 + 8bit 温度整数 + 8bit 温度小数 + 8bit 校验 |

### 6.2 单总线通信时序

```
主机起始信号       传感器应答         40 bit 数据传输
  ┌──┐          ┌──┐  ┌──┐      ┌─────────────────────────┐
  │  │          │  │  │  │      │  每 bit: 50us低 + 高电平   │
──┘  └──────────┘  └──┘  └──────┘   26~28us=0  70us=1       │
  拉低18-20ms   80us 80us          共 40 bit ≈ 4ms          │
   释放20-40us   低    高                                    │
                                   └─────────────────────────┘
```

### 6.3 驱动代码关键设计点

| 设计要素 | 代码实现 | 说明 |
|---------|---------|------|
| GPIO 模式 | `GPIO_MODE_INPUT_OUTPUT_OD` | 开漏模式，主机和传感器共享数据线 |
| 上拉电阻 | `GPIO_PULLUP_ENABLE` | 启用内部上拉（约 45KΩ），外部上拉更可靠 |
| 时序保护 | `portENTER_CRITICAL(&s_timing_mux)` | 临界区禁用调度，保护微秒级时序 |
| 0/1 判定阈值 | `DHT11_ONE_THRESHOLD_US = 50` | 高电平 >50μs 为 "1"，<50μs 为 "0" |
| 超时保护 | `DHT11_EDGE_TIMEOUT_US = 120` | 每个边沿最多等 120μs |

---

## 七、接线示意图

### 7.1 使用 3 针模块板（推荐）

```
ESP32-S3-CAM                         DHT11 模块 (3针)
┌──────────────────┐                 ┌────────────┐
│                  │                 │            │
│     3.3V     ●───┼─────────────────┼──● VCC     │
│                  │                 │            │
│     GPIO38   ●───┼─────────────────┼──● DATA    │
│                  │                 │            │
│     GND      ●───┼─────────────────┼──● GND     │
│                  │                 │            │
└──────────────────┘                 └────────────┘
```

### 7.2 使用 4 针裸传感器（需外接上拉电阻）

```
ESP32-S3-CAM                         DHT11 (4针裸)
┌──────────────────┐       ┌─────┐  ┌────────────┐
│                  │       │4.7KΩ│  │            │
│     3.3V     ●───┼───┬───┤     ├──┼──● Pin2    │
│                  │   │   │     │  │    (DATA)  │
│     GPIO38   ●───┼───┘   └─────┘  │            │
│                  │                 │  ● Pin3    │
│                  │                 │    (NC)    │
│     GND      ●───┼─────────────────┼──● Pin4    │
│                  │                 │    (GND)   │
│     3.3V     ●───┼─────────────────┼──● Pin1    │
│                  │                 │    (VCC)   │
└──────────────────┘                 └────────────┘
```

---

## 八、常见问题 FAQ

### Q1: 为什么编译不报错但运行时读不到数据？

`gpio_config()` API 不检查引脚是否被其他外设占用或者硬件是否连接。GPIO 编号合法就返回 `ESP_OK`。实际硬件通信失败只会在 `dht11_read()` 时以超时形式返回。

### Q2: ESP32-S3 内部上拉电阻够用吗？

ESP32-S3 的 GPIO 内部上拉电阻约 **45KΩ**，对于 DHT11 来说偏大。DHT11 数据手册推荐 **4.7KΩ~10KΩ** 外部上拉。3 针模块板一般已板载 10KΩ 上拉，足够；4 针裸传感器必须自行外接。如果仅依赖内部上拉，短距离可能勉强工作，但不稳定。

### Q3: DHT11 能接 5V 电源吗？

DHT11 支持 3.3V~5.5V 供电。但如果用 5V 供电，DATA 线的高电平为 5V，而 ESP32-S3 的 GPIO 耐压为 3.3V，**可能损坏 GPIO**。建议统一使用 3.3V。

### Q4: 两个任务（DHT11 和 YOLO 推理）都在 Core 1 会冲突吗？

`portENTER_CRITICAL()` 在 DHT11 数据交换阶段（约 4ms）禁用了 Core 1 的任务切换，保护了微秒时序。起始信号阶段（20ms 拉低）在临界区外，理论上可被抢占但不影响协议正确性。不过如果推理任务长期占满 Core 1（4~5秒），DHT11 任务的 3 秒读取周期可能被频繁延迟。

### Q5: 校验和报错是什么原因？

`Checksum mismatch` 说明 40 bit 数据传输中有位被干扰。常见原因：
- 接线过长（>20cm）
- 缺少上拉电阻或上拉值过大
- 电源纹波大（建议在 VCC-GND 间加 100nF 去耦电容）
- 附近有高频干扰源（如电机、开关电源）

### Q6: 如何确认开发板上 GPIO38 对应哪个物理排针？

不同厂家的 ESP32-S3-CAM 开发板排针标注可能不同。务必对照开发板原理图或厂家提供的引脚映射图确认 GPIO38 的物理位置。**丝印标注的数字不一定等于 GPIO 编号。**

---

## 九、排查结论模板

完成排查后，根据实际发现的问题填写：

| 排查层 | 检查项 | 结果 |
|--------|--------|------|
| 第 1 层 | 串口日志中 DHT11 读取是否成功？ | ☐ 成功 / ☐ 超时 / ☐ 校验错 / ☐ 无日志 |
| 第 2 层 | DATA 线确实连接到 GPIO38 物理引脚？ | ☐ 是 / ☐ 否（实际接到 ___） |
| 第 2 层 | VCC 接 3.3V？ | ☐ 是 / ☐ 否（实际接 ___） |
| 第 2 层 | 有外部上拉电阻 / 模块板自带？ | ☐ 有 / ☐ 无 |
| 第 2 层 | GND 共地？ | ☐ 是 / ☐ 否 |
| 第 2.5 层 | DHT11 任务切换到 Core 0 后是否改善？ | ☐ 改善 / ☐ 无变化 |
| 第 3 层 | 浏览器直接访问 /api/sensors 返回什么？ | ☐ ok:true / ☐ ok:false |
| 第 4 层 | 浏览器控制台 fetch 结果？ | ☐ 正常 / ☐ 异常 |

---

## 十、总结

1. **软件代码链路完整正确**，从驱动到前端无遗漏
2. **GPIO38 在 ESP32-S3 上是可用的通用 IO**（经 ESP-IDF v5.4 源码 `spi_pins.h` 确认：MSPI 占用 GPIO26~37，GPIO38 不在其中）
3. 网页显示 `--.-°C` 的唯一可能是 `dht11_read()` 运行时返回错误
4. 需要通过**串口日志**确定是超时、校验错误还是任务未启动，然后针对性排查**硬件接线、上拉电阻、引脚物理位置**等
5. 如果硬件接线确认无误，可尝试将 DHT11 任务从 Core 1 移到 Core 0 以排除与推理任务的调度竞争
