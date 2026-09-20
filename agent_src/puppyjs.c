#include "frida-gumjs.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>

#define LOG_TAG "[PuppyJS]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static GumScript *puppy_script = NULL;

static void on_message(const gchar *message, GBytes *data, gpointer user_data) {
    JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, message, -1, NULL);
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));

    const gchar *type = json_object_get_string_member(root, "type");
    if (strcmp(type, "log") == 0) {
        const gchar *log_message = json_object_get_string_member(root, "payload");
        LOGI("%s", log_message);
    } else {
        LOGI("Message: %s", message);
    }
    g_object_unref(parser);
}

void apply_gumjs_hooks() {
    LOGI("PuppyJS waking up to handle Java-layer metric & RAM mocks...");

    GumScriptBackend *backend = gum_script_backend_obtain_qjs();
    GCancellable *cancellable = NULL;
    GError *error = NULL;

    GString *js = g_string_new("");
    // ============================================
    // BLOCK 0: INIT
    // ============================================
    g_string_append(js,
        "Java.perform(function() {\n"
        "  console.log('[+] Frida JS attaching Java hooks for Display & RAM...');\n"
        "\n"
    );

    // ============================================
    // BLOCK 1: Display Metrics Hooks
    // ============================================
    #ifdef SPECS_WIDTH
    g_string_append_printf(js, "const SPECS_WIDTH = %d;\n", SPECS_WIDTH);
    #endif

    #ifdef SPECS_HEIGHT
    g_string_append_printf(js, "const SPECS_HEIGHT = %d;\n", SPECS_HEIGHT);
    #endif

    #ifdef SPECS_DENSITY
    // Handle both numeric float or string format safely
    g_string_append_printf(js, "const SPECS_DENSITY = %s;\n", SPECS_DENSITY);
    #endif

    #ifdef SPECS_DENSITY_DPI
    g_string_append_printf(js, "const SPECS_DENSITY_DPI = %d;\n", SPECS_DENSITY_DPI);
    #endif

    #ifdef SPECS_XDPI
    g_string_append_printf(js, "const SPECS_XDPI = %s;\n", SPECS_XDPI);
    #endif

    #ifdef SPECS_YDPI
    g_string_append_printf(js, "const SPECS_YDPI = %s;\n", SPECS_YDPI);
    #endif

    // --- Memory & Storage Constants ---
    #ifdef SPECS_TOTAL_RAM
    g_string_append_printf(js, "const SPECS_TOTAL_RAM = %llu;\n", SPECS_TOTAL_RAM);
    #endif

    #ifdef SPECS_RAM_THRESHOLD
    g_string_append_printf(js, "const SPECS_RAM_THRESHOLD = %llu;\n", SPECS_RAM_THRESHOLD);
    #endif

    #ifdef SPECS_AVAIL_RAM
    g_string_append_printf(js, "const SPECS_AVAIL_RAM = %llu;\n", SPECS_AVAIL_RAM);
    #endif

    #if defined(SPECS_TOTAL_SPACE) || defined(SPECS_FREE_SPACE)
    g_string_append(js,
        "const IoFile = Java.use('java.io.File');\n"
        "const StatFs = Java.use('android.os.StatFs');\n"
    );
    #ifdef SPECS_TOTAL_SPACE
    g_string_append_printf(js, "const SPECS_TOTAL_SPACE = %llu;\n", SPECS_TOTAL_SPACE);
    #endif
    #ifdef SPECS_FREE_SPACE
    g_string_append_printf(js, "const SPECS_FREE_SPACE = %llu;\n", SPECS_FREE_SPACE);
    #endif
    #endif

    #if defined(SPECS_WIDTH) || defined(SPECS_HEIGHT) || defined(SPECS_DENSITY) || \
        defined(SPECS_DENSITY_DPI) || defined(SPECS_XDPI) || defined(SPECS_YDPI)
    g_string_append(js,
        "  try {\n"
        "    const Display = Java.use('android.view.Display');\n"
        "    const patchMetrics = function(metrics) {\n"
        "      if (metrics !== null) {\n"
    );

    #ifdef SPECS_WIDTH
    g_string_append(js, "        metrics.widthPixels.value = SPECS_WIDTH;\n");
    #endif
    #ifdef SPECS_HEIGHT
    g_string_append(js, "        metrics.heightPixels.value = SPECS_HEIGHT;\n");
    #endif
    #ifdef SPECS_DENSITY
    g_string_append(js, "        metrics.density.value = SPECS_DENSITY;\n");
    #endif
    #ifdef SPECS_DENSITY_DPI
    g_string_append(js, "        metrics.densityDpi.value = SPECS_DENSITY_DPI;\n");
    #endif
    #ifdef SPECS_XDPI
    g_string_append(js, "        metrics.xdpi.value = SPECS_XDPI;\n");
    #endif
    #ifdef SPECS_YDPI
    g_string_append(js, "        metrics.ydpi.value = SPECS_YDPI;\n");
    #endif

    g_string_append(js,
        "      }\n"
        "    };\n"
        "    Display.getRealMetrics.overload('android.util.DisplayMetrics').implementation = function(metrics) {\n"
        "      this.getRealMetrics(metrics);\n"
        "      patchMetrics(metrics);\n"
        "    };\n"
        "    Display.getMetrics.overload('android.util.DisplayMetrics').implementation = function(metrics) {\n"
        "      this.getMetrics(metrics);\n"
        "      patchMetrics(metrics);\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking Display metrics: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 2: Jitter Utility Function
    // ============================================
    g_string_append(js,
        "  function getRealisticJitter(baseValue, variancePercent) {\n"
        "    const variance = baseValue * variancePercent;\n"
        "    const offset = (Math.random() * variance * 2) - variance;\n"
        "    return Math.floor(baseValue + offset);\n"
        "  }\n"
        "\n"
    );

    // ============================================
    // BLOCK: Total Storage Space Hooks
    // ============================================
    #ifdef SPECS_TOTAL_SPACE
    g_string_append(js,
        "  try {\n"
        "    IoFile.getTotalSpace.implementation = function () {\n"
        "      return SPECS_TOTAL_SPACE;\n"
        "    };\n"
        "    StatFs.getTotalBytes.implementation = function () {\n"
        "      return SPECS_TOTAL_SPACE;\n"
        "    };\n"
        "    StatFs.getBlockCount.implementation = function() {\n"
        "      return Math.floor(SPECS_TOTAL_SPACE / this.getBlockSize());\n"
        "    };\n"
        "    StatFs.getBlockCountLong.implementation = function() {\n"
        "      return Math.floor(SPECS_TOTAL_SPACE / this.getBlockSizeLong());\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking total storage space: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 4: Storage Space Hooks (Free)
    // ============================================
    #ifdef SPECS_FREE_SPACE
    g_string_append(js,
        "  try {\n"
        "    IoFile.getFreeSpace.implementation = function () {\n"
        "      return getRealisticJitter(SPECS_FREE_SPACE, 0.0005);\n"
        "    };\n"
        "    StatFs.getAvailableBytes.implementation = function () {\n"
        "      return getRealisticJitter(SPECS_FREE_SPACE, 0.0005);\n"
        "    };\n"
        "    StatFs.getFreeBytes.implementation = function () {\n"
        "      return getRealisticJitter(SPECS_FREE_SPACE, 0.0005);\n"
        "    };\n"
        "    StatFs.getAvailableBlocks.implementation = function () {\n"
        "      const jitteredFreeSpace = getRealisticJitter(SPECS_FREE_SPACE, 0.0005);\n"
        "      return Math.floor(jitteredFreeSpace / this.getBlockSize());\n"
        "    };\n"
        "    StatFs.getAvailableBlocksLong.implementation = function () {\n"
        "      const jitteredFreeSpace = getRealisticJitter(SPECS_FREE_SPACE, 0.0005);\n"
        "      return Math.floor(jitteredFreeSpace / this.getBlockSizeLong());\n"
        "    };\n"
        "    StatFs.getFreeBlocks.implementation = function () {\n"
        "      const jitteredFreeSpace = getRealisticJitter(SPECS_FREE_SPACE, 0.0005);\n"
        "      return Math.floor(jitteredFreeSpace / this.getBlockSize());\n"
        "    };\n"
        "    StatFs.getFreeBlocksLong.implementation = function () {\n"
        "      const jitteredFreeSpace = getRealisticJitter(SPECS_FREE_SPACE, 0.0005);\n"
        "      return Math.floor(jitteredFreeSpace / this.getBlockSizeLong());\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking free storage space: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 5: RAM Memory Hooks
    // ============================================
    #if defined(SPECS_TOTAL_RAM) || defined(SPECS_RAM_THRESHOLD) || defined(SPECS_AVAIL_RAM)
    g_string_append(js,
        "  try {\n"
        "    const ActivityManager = Java.use('android.app.ActivityManager');\n"
        "    ActivityManager.getMemoryInfo.implementation = function(mem) {\n"
        "      this.getMemoryInfo(mem);\n"
        "      if (mem !== null) {\n"
    );

    #ifdef SPECS_TOTAL_RAM
    g_string_append(js, "        mem.totalMem.value = SPECS_TOTAL_RAM;\n");
    #endif

    #ifdef SPECS_RAM_THRESHOLD
    g_string_append(js, "        mem.threshold.value = SPECS_RAM_THRESHOLD;\n");
    #endif

    #ifdef SPECS_AVAIL_RAM
    g_string_append(js, "        mem.availMem.value = getRealisticJitter(SPECS_AVAIL_RAM, 0.03);\n");
    #endif

    g_string_append(js,
        "        mem.lowMemory.value = false;\n"
        "      }\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking ActivityManager MemoryInfo: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 6: Battery Cloaking Hooks
    // ============================================
    #ifdef SPECS_BATTERY_LEVEL
    g_string_append(js,
        "  try {\n"
        "    const BatteryManager = Java.use('android.os.BatteryManager');\n"
        "    const Intent = Java.use('android.content.Intent');\n"
        "    const Bundle = Java.use('android.os.Bundle');\n"
    );

    g_string_append_printf(js, "    const battery_level = %d;\n", SPECS_BATTERY_LEVEL);

    g_string_append(js,
        "    const jitter = Math.floor(Math.random() * 50) - 25;\n"
        "    const spoof_voltage = Math.floor(3500 + (battery_level * 8.5) + jitter);\n"
        "    const spoof_temp = Math.floor(Math.random() * 81) + 260;\n"
        "\n"
        "    const getSpoofedBattery = function(id, val) {\n"
        "      if (id === 4) return battery_level;\n"
        "      if (id === 6) return 3;\n"
        "      if (id === 2 || id === 3) return -450000;\n"
        "      if ((id === 1 || id === 5) && val == 0) return (id === 1) ? 2000000 : 7000000000;\n"
        "      return val;\n"
        "    };\n"
        "\n"
        "    if (BatteryManager.getIntProperty) {\n"
        "      BatteryManager.getIntProperty.overload('int').implementation = function(id) {\n"
        "        return Number(getSpoofedBattery(id, this.getIntProperty(id)));\n"
        "      };\n"
        "    }\n"
        "    if (BatteryManager.getLongProperty) {\n"
        "      BatteryManager.getLongProperty.overload('int').implementation = function(id) {\n"
        "        return Number(getSpoofedBattery(id, this.getLongProperty(id)));\n"
        "      };\n"
        "    }\n"
        "    if (BatteryManager.isCharging) {\n"
        "      BatteryManager.isCharging.implementation = function() { return false; };\n"
        "    }\n"
        "    if (BatteryManager.computeChargeTimeRemaining) {\n"
        "      BatteryManager.computeChargeTimeRemaining.implementation = function() { return -1; };\n"
        "    }\n"
        "\n"
        "    Intent.getIntExtra.overloads.forEach(function(overload) {\n"
        "      overload.implementation = function() {\n"
        "        const ret = overload.apply(this, arguments);\n"
        "        const key = arguments[0];\n"
        "        const map = {'plugged': 0, 'status': 3, 'level': battery_level, 'scale': ret === 0 ? 100 : ret, 'health': 2, 'voltage': ret === 0 ? spoof_voltage : ret, 'temperature': ret === 0 ? spoof_temp : ret};\n"
        "        return map[key] !== undefined ? map[key] : ret;\n"
        "      };\n"
        "    });\n"
        "\n"
        "    Bundle.getInt.overloads.forEach(function(overload) {\n"
        "      overload.implementation = function() {\n"
        "        const ret = overload.apply(this, arguments);\n"
        "        const key = arguments[0];\n"
        "        const map = {'plugged': 0, 'status': 3, 'level': battery_level, 'scale': ret === 0 ? 100 : ret, 'health': 2, 'voltage': ret === 0 ? spoof_voltage : ret, 'temperature': ret === 0 ? spoof_temp : ret};\n"
        "        return map[key] !== undefined ? map[key] : ret;\n"
        "      };\n"
        "    });\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking battery info: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 7: Time Cloaking Hooks
    // ============================================
    #ifdef SYS_PROP_PERSIST_SYS_TIMEZONE
    g_string_append(js,
        "  try {\n"
        "    const Date = Java.use('java.util.Date');\n"
        "    const Calendar = Java.use('java.util.Calendar');\n"
        "    const TimeZone = Java.use('java.util.TimeZone');\n"
    );

    g_string_append_printf(js, "    const timeZoneID = '%s';\n", SYS_PROP_PERSIST_SYS_TIMEZONE);

    g_string_append(js,
        "\n"
        "    Date.after.overload('java.util.Date').implementation = function(date) {\n"
        "      return true;\n"
        "    };\n"
        "\n"
        "    Calendar.getInstance.overload().implementation = function() {\n"
        "      const cal = this.getInstance();\n"
        "      cal.setTimeZone(TimeZone.getTimeZone(timeZoneID));\n"
        "      return cal;\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking time cloaking: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 8: OpenGL Cloaking Hooks
    // ============================================
    #if defined(DEVICE_GL_VENDOR) || defined(DEVICE_GL_RENDERER) || defined(DEVICE_GL_VERSION)
    g_string_append(js,
        "  try {\n"
        "    const glMap = {};\n"
    );

    #ifdef DEVICE_GL_VENDOR
    g_string_append_printf(js, "    glMap[0x1F00] = '%s';\n", DEVICE_GL_VENDOR);
    #endif

    #ifdef DEVICE_GL_RENDERER
    g_string_append_printf(js, "    glMap[0x1F01] = '%s';\n", DEVICE_GL_RENDERER);
    #endif

    #ifdef DEVICE_GL_VERSION
    g_string_append_printf(js, "    glMap[0x1F02] = '%s';\n", DEVICE_GL_VERSION);
    #endif

    g_string_append(js,
        "    const glClasses = ['android.opengl.GLES20', 'android.opengl.GLES30', 'com.google.android.gles_jni.GLImpl'];\n"
        "\n"
        "    glClasses.forEach(function(clsName) {\n"
        "      try {\n"
        "        const cls = Java.use(clsName);\n"
        "        if (cls && cls.glGetString) {\n"
        "          cls.glGetString.implementation = function(name) {\n"
        "            return glMap[name] !== undefined ? glMap[name] : this.glGetString(name);\n"
        "          };\n"
        "        }\n"
        "      } catch (e) {}\n"
        "    });\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking OpenGL: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 9: Vulkan Cloaking Hooks
    // ============================================
    #ifdef DEVICE_GL_RENDERER
    g_string_append(js,
        "  try {\n"
    );

    g_string_append_printf(js, "    const fakeRenderer = '%s';\n", DEVICE_GL_RENDERER);

    g_string_append(js,
        "    const libvulkan = Process.getModuleByName('libvulkan.so');\n"
        "    if (!libvulkan) {\n"
        "      console.log('[-] libvulkan.so not loaded (OpenGL only)');\n"
        "    } else {\n"
        "      let isPropertiesHooked = false;\n"
        "      Interceptor.attach(libvulkan.getExportByName('vkGetInstanceProcAddr'), {\n"
        "        onEnter: function(args) {\n"
        "          this.funcName = args[1].readCString();\n"
        "        },\n"
        "        onLeave: function(retval) {\n"
        "          if (this.funcName === 'vkGetPhysicalDeviceProperties' && !retval.isNull() && !isPropertiesHooked) {\n"
        "            isPropertiesHooked = true;\n"
        "            Interceptor.attach(retval, {\n"
        "              onEnter: function(args) {\n"
        "                this.pProperties = args[1];\n"
        "              },\n"
        "              onLeave: function(retval) {\n"
        "                this.pProperties.add(20).writeUtf8String(fakeRenderer);\n"
        "                this.pProperties.add(0x8).writeU32(0x5143);\n"
        "              }\n"
        "            });\n"
        "          }\n"
        "        }\n"
        "      });\n"
        "    }\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking Vulkan: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 10: Install Referrer Bypass Hooks
    // ============================================
    #ifdef ATTRIBUTION_INSTALL_REFERRER
    g_string_append(js,
        "  try {\n"
    );

    g_string_append_printf(js, "    const installReferrer = '%s';\n", ATTRIBUTION_INSTALL_REFERRER);

    g_string_append(js,
        "    const cachedInstallTime = 1609459200;\n"
        "    const cachedClickTime = 1609459200;\n"
        "\n"
        "    try {\n"
        "      const ReferrerDetails = Java.use('com.android.installreferrer.api.ReferrerDetails');\n"
        "      if (ReferrerDetails && ReferrerDetails.getInstallReferrer) {\n"
        "        ReferrerDetails.getInstallReferrer.implementation = function() {\n"
        "          return installReferrer;\n"
        "        };\n"
        "      }\n"
        "    } catch (e) {}\n"
        "\n"
        "    const Bundle = Java.use('android.os.Bundle');\n"
        "\n"
        "    Bundle.getString.overloads.forEach(function(overload) {\n"
        "      overload.implementation = function() {\n"
        "        const key = arguments[0];\n"
        "        const ret = overload.apply(this, arguments);\n"
        "        return key === 'install_referrer' ? installReferrer : ret;\n"
        "      };\n"
        "    });\n"
        "\n"
        "    Bundle.getLong.overloads.forEach(function(overload) {\n"
        "      overload.implementation = function() {\n"
        "        const key = arguments[0];\n"
        "        const ret = overload.apply(this, arguments);\n"
        "        const timeMap = {\n"
        "          'install_begin_timestamp_seconds': cachedInstallTime,\n"
        "          'install_begin_timestamp_server_seconds': cachedInstallTime,\n"
        "          'referrer_click_timestamp_seconds': cachedClickTime,\n"
        "          'referrer_click_timestamp_server_seconds': cachedClickTime\n"
        "        };\n"
        "        return timeMap[key] !== undefined ? timeMap[key] : ret;\n"
        "      };\n"
        "    });\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking install referrer: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 11: USB Action Bypass Hooks
    // ============================================
    g_string_append(js,
        "  try {\n"
        "    const Intent = Java.use('android.content.Intent');\n"
        "    const usbAction = 'android.hardware.usb.action.USB_STATE';\n"
        "    const usbKeys = {'connected': true, 'adb': true, 'configured': true};\n"
        "\n"
        "    Intent.getExtras.overload().implementation = function() {\n"
        "      const bundle = this.getExtras();\n"
        "      if (this.getAction() === usbAction && bundle !== null) {\n"
        "        bundle.putBoolean('connected', false);\n"
        "        bundle.putBoolean('adb', false);\n"
        "        bundle.putBoolean('configured', false);\n"
        "      }\n"
        "      return bundle;\n"
        "    };\n"
        "\n"
        "    Intent.getBooleanExtra.overload('java.lang.String', 'boolean').implementation = function(key, defaultVal) {\n"
        "      if (this.getAction() === usbAction && usbKeys[key]) {\n"
        "        return false;\n"
        "      }\n"
        "      return this.getBooleanExtra(key, defaultVal);\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking USB action: ' + e);\n"
        "  }\n"
        "\n"
    );

    // ============================================
    // BLOCK 12: Root Check Bypass Hooks
    // ============================================
    g_string_append(js,
        "  try {\n"
        "    const Runtime = Java.use('java.lang.Runtime');\n"
        "    const ProcessBuilder = Java.use('java.lang.ProcessBuilder');\n"
        "    const badCommands = ['su', 'magisk', 'frida', 'xposed', 'root', 'busybox'];\n"
        "    const fakeCommand = '/system/bin/echo';\n"
        "\n"
        "    const sanitizeCmd = function(cmd) {\n"
        "      if (!cmd) return cmd;\n"
        "      for (let i = 0; i < badCommands.length; i++) {\n"
        "        if (cmd.indexOf(badCommands[i]) !== -1) {\n"
        "          return fakeCommand;\n"
        "        }\n"
        "      }\n"
        "      return cmd;\n"
        "    };\n"
        "\n"
        "    const getListStr = function(list) {\n"
        "      if (!list) return '';\n"
        "      let str = '';\n"
        "      for (let i = 0; i < list.size(); i++) {\n"
        "        let item = list.get(i);\n"
        "        if (item) str += item.toString() + ' ';\n"
        "      }\n"
        "      return str.trim();\n"
        "    };\n"
        "\n"
        "    Runtime.exec.overloads.forEach(function(overload) {\n"
        "      overload.implementation = function() {\n"
        "        let args = [].slice.call(arguments);\n"
        "        let cmd = args[0];\n"
        "        if (typeof cmd === 'string') {\n"
        "          args[0] = sanitizeCmd(cmd);\n"
        "        } else if (cmd && typeof cmd === 'object' && 'length' in cmd) {\n"
        "          for (let i = 0; i < cmd.length; i++) {\n"
        "            cmd[i] = sanitizeCmd(cmd[i]);\n"
        "          }\n"
        "        }\n"
        "        return this.exec.apply(this, args);\n"
        "      };\n"
        "    });\n"
        "\n"
        "    ProcessBuilder.command.overload('java.util.List').implementation = function(list) {\n"
        "      if (list && list.size() > 0) {\n"
        "        for (let i = 0; i < list.size(); i++) {\n"
        "          let cmd = list.get(i).toString();\n"
        "          let sanitized = sanitizeCmd(cmd);\n"
        "          if (cmd !== sanitized) list.set(i, sanitized);\n"
        "        }\n"
        "      }\n"
        "      return this.command.call(this, list);\n"
        "    };\n"
        "\n"
        "    ProcessBuilder.command.overload('[Ljava.lang.String;').implementation = function(arr) {\n"
        "      if (arr && arr.length > 0) {\n"
        "        for (let i = 0; i < arr.length; i++) {\n"
        "          arr[i] = sanitizeCmd(arr[i]);\n"
        "        }\n"
        "      }\n"
        "      return this.command.call(this, arr);\n"
        "    };\n"
        "\n"
        "    ProcessBuilder.start.implementation = function() {\n"
        "      let cmdList = this.command();\n"
        "      if (cmdList && cmdList.size() > 0) {\n"
        "        for (let i = 0; i < cmdList.size(); i++) {\n"
        "          let cmd = cmdList.get(i).toString();\n"
        "          let sanitized = sanitizeCmd(cmd);\n"
        "          if (cmd !== sanitized) cmdList.set(i, sanitized);\n"
        "        }\n"
        "      }\n"
        "      return this.start.call(this);\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking root checks: ' + e);\n"
        "  }\n"
        "\n"
    );

    // ============================================
    // BLOCK 13: FLAG_SECURE Bypass Hooks
    // ============================================
    #ifdef BYPASS_FLAG_SECURE
    g_string_append(js,
        "  try {\n"
        "    const Window = Java.use('android.view.Window');\n"
        "    const LayoutParams = Java.use('android.view.WindowManager$LayoutParams');\n"
        "    const FLAG_SECURE = 8192;\n"
        "\n"
        "    Window.addFlags.overload('int').implementation = function(flag) {\n"
        "      if (flag === FLAG_SECURE) return;\n"
        "      this.addFlags(flag);\n"
        "    };\n"
        "\n"
        "    Window.setFlags.overload('int', 'int').implementation = function(flags, mask) {\n"
        "      flags = flags & ~LayoutParams.FLAG_SECURE.value;\n"
        "      this.setFlags(flags, mask);\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking FLAG_SECURE: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 14: Telephony Carrier Cloaking Hooks
    // ============================================
    #if defined(TELECOM_MCC) || defined(TELECOM_MNC) || defined(TELECOM_OPERATOR_NAME) || \
        defined(TELECOM_COUNTRY_ISO) || defined(TELECOM_LANGUAGE) || defined(TELECOM_PHONE_CODE) || \
        defined(TELECOM_SIM_STATE_NUM) || defined(TELECOM_MCC_COUNTRY_NAME) || \
        defined(TELECOM_IMEI_PREFIX) || defined(TELECOM_NETWORK_TYPE)
    g_string_append(js,
        "  try {\n"
        "    const TelephonyManager = Java.use('android.telephony.TelephonyManager');\n"
    );

    #if (defined(TELECOM_MCC) && defined(TELECOM_MNC)) || defined(TELECOM_PHONE_CODE) || defined(TELECOM_IMEI_PREFIX)
    g_string_append(js,
        "    const randomStr = function(len) {\n"
        "      let out = '';\n"
        "      for (let i = 0; i < len; i++) {\n"
        "        out += Math.floor(Math.random() * 10).toString();\n"
        "      }\n"
        "      return out;\n"
        "    };\n"
        "\n"
    );
    #endif

    #ifdef TELECOM_MCC
    g_string_append_printf(js, "    const mcc = '%s';\n", TELECOM_MCC);
    #endif

    #ifdef TELECOM_MNC
    g_string_append_printf(js, "    const mnc = '%s';\n", TELECOM_MNC);
    #endif

    #if defined(TELECOM_MCC) && defined(TELECOM_MNC)
    g_string_append(js,
        "    const mccmnc = mcc + mnc;\n"
        "    const subscriberId = mccmnc + randomStr(15 - mccmnc.length);\n"
    );
    #endif

    #ifdef TELECOM_OPERATOR_NAME
    g_string_append_printf(js, "    const operatorName = '%s';\n", TELECOM_OPERATOR_NAME);
    #endif

    #ifdef TELECOM_COUNTRY_ISO
    g_string_append_printf(js, "    const countryIso = '%s';\n", TELECOM_COUNTRY_ISO);
    #endif

    #ifdef TELECOM_LANGUAGE
    g_string_append_printf(js, "    const language = '%s';\n", TELECOM_LANGUAGE);
    #endif

    #ifdef TELECOM_PHONE_CODE
    g_string_append_printf(js, "    const phoneNumber = '%s' + randomStr(10);\n", TELECOM_PHONE_CODE);
    #endif

    #ifdef TELECOM_SIM_STATE_NUM
    g_string_append_printf(js, "    const simStateNum = %d;\n", TELECOM_SIM_STATE_NUM);
    #endif

    #ifdef TELECOM_MCC_COUNTRY_NAME
    g_string_append_printf(js, "    const countryName = '%s';\n", TELECOM_MCC_COUNTRY_NAME);
    #endif

    #ifdef TELECOM_IMEI_PREFIX
    g_string_append_printf(js, "    const imei = '%s' + randomStr(13);\n", TELECOM_IMEI_PREFIX);
    #endif

    #ifdef TELECOM_NETWORK_TYPE
    g_string_append_printf(js, "    const networkType = '%s';\n", TELECOM_NETWORK_TYPE);
    #endif

    g_string_append(js, "    const telMap = {};\n");

    #if defined(TELECOM_MCC) && defined(TELECOM_MNC)
    g_string_append(js,
        "    telMap['getSimOperator'] = mccmnc;\n"
        "    telMap['getNetworkOperator'] = mccmnc;\n"
        "    telMap['getSubscriberId'] = subscriberId;\n"
    );
    #endif

    #ifdef TELECOM_OPERATOR_NAME
    g_string_append(js,
        "    telMap['getSimOperatorName'] = operatorName;\n"
        "    telMap['getNetworkOperatorName'] = operatorName;\n"
    );
    #endif

    #ifdef TELECOM_COUNTRY_ISO
    g_string_append(js,
        "    telMap['getSimCountryIso'] = countryIso;\n"
        "    telMap['getNetworkCountryIso'] = countryIso;\n"
    );
    #endif

    #ifdef TELECOM_PHONE_CODE
    g_string_append(js, "    telMap['getLine1Number'] = phoneNumber;\n");
    #endif

    #ifdef TELECOM_IMEI_PREFIX
    g_string_append(js, "    telMap['getDeviceId'] = imei;\n");
    #endif

    g_string_append(js,
        "    Object.keys(telMap).forEach(function(method) {\n"
        "      if (TelephonyManager[method]) {\n"
        "        TelephonyManager[method].overload().implementation = function() {\n"
        "          return telMap[method];\n"
        "        };\n"
        "      }\n"
        "    });\n"
    );

    #ifdef TELECOM_SIM_STATE_NUM
    g_string_append(js,
        "    if (TelephonyManager.getSimState) {\n"
        "      TelephonyManager.getSimState.overload().implementation = function() {\n"
        "        return simStateNum;\n"
        "      };\n"
        "    }\n"
    );
    #endif

    #ifdef TELECOM_NETWORK_TYPE
    g_string_append(js,
        "    if (TelephonyManager.getNetworkType) {\n"
        "      TelephonyManager.getNetworkType.overload().implementation = function() {\n"
        "        const typeMap = {'LTE': 13, '5G': 20, '4G': 13, '3G': 3, '2G': 1};\n"
        "        return typeMap[networkType] || 13;\n"
        "      };\n"
        "    }\n"
    );
    #endif

    #if defined(TELECOM_MCC) || defined(TELECOM_MNC) || (defined(TELECOM_LANGUAGE) && defined(TELECOM_COUNTRY_ISO))
    g_string_append(js,
        "    const Locale = Java.use('java.util.Locale');\n"
        "    [Java.use('android.view.ContextThemeWrapper'), Java.use('android.app.ContextImpl')].forEach(function(ctx) {\n"
        "      if (ctx && ctx.getResources) {\n"
        "        ctx.getResources.overload().implementation = function() {\n"
        "          const res = this.getResources();\n"
        "          const config = res.getConfiguration();\n"
    );

    #ifdef TELECOM_MCC
    g_string_append(js, "          config.mcc.value = parseInt(mcc);\n");
    #endif

    #ifdef TELECOM_MNC
    g_string_append(js, "          config.mnc.value = parseInt(mnc);\n");
    #endif

    #if defined(TELECOM_LANGUAGE) && defined(TELECOM_COUNTRY_ISO)
    g_string_append(js, "          config.setLocale(Locale.$new(language, countryIso));\n");
    #endif

    g_string_append(js,
        "          return res;\n"
        "        };\n"
        "      }\n"
        "    });\n"
    );
    #endif

    g_string_append(js,
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking telephony carrier: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 15: Development & Debug Settings Bypass
    // ============================================
    g_string_append(js,
        "  try {\n"
        "    const genHex = function(len) {\n"
        "      let hex = '';\n"
        "      for (let i = 0; i < len; i++) {\n"
        "        hex += Math.floor(Math.random() * 16).toString(16);\n"
        "      }\n"
        "      return hex;\n"
        "    };\n"
        "\n"
        "    const fakeId = genHex(16);\n"
        "    const settingsMap = {\n"
        "      'android_id': fakeId,\n"
        "      'mobile_data_always_on': '0',\n"
        "      'adb_enabled': '0',\n"
        "      'adb_wifi_enabled': '0',\n"
        "      'development_settings_enabled': '0',\n"
        "      'install_non_market_apps': '0'\n"
        "    };\n"
    );

    #ifdef SYS_PROP_RO_PRODUCT_MODEL
    g_string_append_printf(js, "    settingsMap['device_name'] = '%s';\n", SYS_PROP_RO_PRODUCT_MODEL);
    #endif

    #ifdef SYS_PROP_RO_BUILD_DISPLAY_ID
    g_string_append_printf(js, "    settingsMap['database_creation_buildid'] = '%s';\n", SYS_PROP_RO_BUILD_DISPLAY_ID);
    #endif

    #ifdef SYS_PROP_PERSIST_SYS_LOCALE
    g_string_append_printf(js, "    settingsMap['system_locales'] = '%s';\n", SYS_PROP_PERSIST_SYS_LOCALE);
    #endif

    #ifdef NETWORK_NET_TYPE
    g_string_append_printf(js, "    settingsMap['wifi_on'] = (%d === 1) ? '1' : '0';\n", NETWORK_NET_TYPE);
    g_string_append_printf(js, "    settingsMap['mobile_data'] = (%d === 0) ? '1' : '0';\n", NETWORK_NET_TYPE);
    #endif

    #ifdef DEVICE_KEYBOARD
    g_string_append_printf(js, "    settingsMap['default_input_method'] = '%s';\n", DEVICE_KEYBOARD);
    #endif

    g_string_append(js,
        "\n"
        "    const classConfigs = {\n"
        "      'android.provider.Settings$Global': ['device_name', 'mobile_data', 'mobile_data_always_on', 'database_creation_buildid', 'adb_wifi_enabled', 'development_settings_enabled', 'adb_enabled', 'install_non_market_apps', 'wifi_on'],\n"
        "      'android.provider.Settings$Secure': ['android_id', 'mobile_data', 'development_settings_enabled', 'adb_enabled', 'install_non_market_apps', 'wifi_on', 'default_input_method'],\n"
        "      'android.provider.Settings$System': ['system_locales', 'android_id', 'adb_enabled', 'install_non_market_apps', 'wifi_on']\n"
        "    };\n"
        "\n"
        "    Object.keys(classConfigs).forEach(function(className) {\n"
        "      const clazz = Java.use(className);\n"
        "      const allowedKeys = classConfigs[className];\n"
        "\n"
        "      if (clazz.getString) {\n"
        "        clazz.getString.overloads.forEach(function(overload) {\n"
        "          overload.implementation = function() {\n"
        "            const name = arguments[1];\n"
        "            if (allowedKeys.indexOf(name) !== -1 && settingsMap[name] !== undefined) {\n"
        "              return settingsMap[name];\n"
        "            }\n"
        "            return this.getString.apply(this, arguments);\n"
        "          };\n"
        "        });\n"
        "      }\n"
        "\n"
        "      if (clazz.getInt) {\n"
        "        clazz.getInt.overloads.forEach(function(overload) {\n"
        "          overload.implementation = function() {\n"
        "            const name = arguments[1];\n"
        "            if (allowedKeys.indexOf(name) !== -1 && settingsMap[name] !== undefined) {\n"
        "              return parseInt(settingsMap[name]);\n"
        "            }\n"
        "            return this.getInt.apply(this, arguments);\n"
        "          };\n"
        "        });\n"
        "      }\n"
        "    });\n"
        "\n"
        "    const VMDebug = Java.use('dalvik.system.VMDebug');\n"
        "    if (VMDebug.lastDebuggerActivity) {\n"
        "      VMDebug.lastDebuggerActivity.overload().implementation = function() {\n"
        "        return -1;\n"
        "      };\n"
        "    }\n"
        "\n"
        "    const Debug = Java.use('android.os.Debug');\n"
        "    if (Debug.isDebuggerConnected) {\n"
        "      Debug.isDebuggerConnected.implementation = function() {\n"
        "        return false;\n"
        "      };\n"
        "    }\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking debug settings: ' + e);\n"
        "  }\n"
        "\n"
    );

    // ============================================
    // BLOCK 16: Locale & Timezone Cloaking Hooks
    // ============================================
    #if defined(SYS_PROP_PERSIST_SYS_LANGUAGE) || defined(SYS_PROP_PERSIST_SYS_LOCALE) || defined(SYS_PROP_PERSIST_SYS_TIMEZONE)
    g_string_append(js, "  try {\n");

    #if defined(SYS_PROP_PERSIST_SYS_LANGUAGE) || defined(SYS_PROP_PERSIST_SYS_LOCALE)
    #ifdef SYS_PROP_PERSIST_SYS_LANGUAGE
    g_string_append_printf(js, "    const language = '%s';\n", SYS_PROP_PERSIST_SYS_LANGUAGE);
    #else
    g_string_append(js, "    const language = 'en';\n");
    #endif

    #ifdef SYS_PROP_PERSIST_SYS_LOCALE
    g_string_append_printf(js, "    const locale = '%s';\n", SYS_PROP_PERSIST_SYS_LOCALE);
    #else
    g_string_append(js, "    const locale = 'en-US';\n");
    #endif

    g_string_append(js,
        "    const Locale = Java.use('java.util.Locale');\n"
        "    const country = locale.split('-')[1] || locale.toUpperCase();\n"
        "\n"
        "    Locale.getDefault.overload().implementation = function() {\n"
        "      return Locale.$new(language, country);\n"
        "    };\n"
        "\n"
        "    const Build = Java.use('android.os.Build$VERSION');\n"
        "    if (Build.SDK_INT.value >= 24) {\n"
        "      const LocaleList = Java.use('android.os.LocaleList');\n"
        "      LocaleList.getDefault.implementation = function() {\n"
        "        return LocaleList.$new(Java.array('java.util.Locale', [Locale.$new(language, country)]));\n"
        "      };\n"
        "    }\n"
    );
    #endif

    #ifdef SYS_PROP_PERSIST_SYS_TIMEZONE
    g_string_append_printf(js, "    const timezone = '%s';\n", SYS_PROP_PERSIST_SYS_TIMEZONE);
    g_string_append(js,
        "    const TimeZone = Java.use('java.util.TimeZone');\n"
        "    TimeZone.getDefault.implementation = function() {\n"
        "      return TimeZone.getTimeZone(timezone);\n"
        "    };\n"
    );
    #endif

    g_string_append(js,
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking locale/timezone: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 17: Network Info Spoofing Hooks
    // ============================================
    #ifdef NETWORK_NET_TYPE
    g_string_append(js,
        "  try {\n"
        "    const NetworkInfo = Java.use('android.net.NetworkInfo');\n"
    );

    g_string_append_printf(js,
        "    const netType = %d;\n"
        "    const typeName = (netType === 1) ? 'WIFI' : 'MOBILE';\n"
        "    const subType = (netType === 1) ? '' : 'LTE';\n"
        "    const fakeNetInfo = NetworkInfo.$new(netType, 0, typeName, subType);\n",
        NETWORK_NET_TYPE
    );

    g_string_append(js,
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking network info: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // BLOCK 18: Network & VPN Bypass Hooks
    // ============================================
    #if defined(NETWORK_NET_TYPE) || defined(NETWORK_MOBILE_DATA_LOCAL_IP) || defined(NETWORK_WIFI_LOCAL_IP)
    g_string_append(js,
        "  try {\n"
    );

    #ifdef NETWORK_NET_TYPE
    g_string_append(js,
        "    const TRANSPORT_CELLULAR = 0, TRANSPORT_WIFI = 1, TRANSPORT_VPN = 4;\n"
        "    const CAP_INTERNET = 12, CAP_NOT_VPN = 15, CAP_VALIDATED = 16;\n"
        "\n"
        "    const NetworkInfo = Java.use('android.net.NetworkInfo');\n"
        "    const NetworkInfoState = Java.use('android.net.NetworkInfo$State');\n"
        "    const NetworkInfoDetailedState = Java.use('android.net.NetworkInfo$DetailedState');\n"
        "    const ConnectivityManager = Java.use('android.net.ConnectivityManager');\n"
        "    const NetworkCapabilities = Java.use('android.net.NetworkCapabilities');\n"
        "    const Builder = Java.use('android.net.NetworkCapabilities$Builder');\n"
        "    const TelephonyManager = Java.use('android.telephony.TelephonyManager');\n"
        "\n"
    );

    g_string_append_printf(js, "    const netType = %d;\n", NETWORK_NET_TYPE);

    g_string_append(js,
        "\n"
        "    const createFakeNetInfo = function() {\n"
        "      const type = netType === 1 ? 'WIFI' : 'MOBILE';\n"
        "      const sub = netType === 1 ? '' : 'LTE';\n"
        "      return NetworkInfo.$new(netType, 0, type, sub);\n"
        "    };\n"
        "\n"
        "    NetworkCapabilities.hasTransport.implementation = function(t) {\n"
        "      if (t === TRANSPORT_VPN) return false;\n"
        "      if (t === TRANSPORT_WIFI && netType === 0) return false;\n"
        "      if (t === TRANSPORT_CELLULAR && netType === 1) return false;\n"
        "      return this.hasTransport(t);\n"
        "    };\n"
        "\n"
        "    ConnectivityManager.isActiveNetworkMetered.implementation = function() {\n"
        "      return netType === 0;\n"
        "    };\n"
        "\n"
        "    NetworkInfo.isConnected.implementation = function() {\n"
        "      return netType === this.getType();\n"
        "    };\n"
        "\n"
        "    NetworkInfo.isConnectedOrConnecting.implementation = function() {\n"
        "      return netType === this.getType();\n"
        "    };\n"
        "\n"
        "    NetworkInfo.getState.implementation = function() {\n"
        "      return netType === this.getType() ? NetworkInfoState.CONNECTED.value : NetworkInfoState.DISCONNECTED.value;\n"
        "    };\n"
        "\n"
        "    NetworkInfo.getDetailedState.implementation = function() {\n"
        "      return netType === this.getType() ? NetworkInfoDetailedState.CONNECTED.value : NetworkInfoDetailedState.DISCONNECTED.value;\n"
        "    };\n"
        "\n"
        "    ConnectivityManager.getActiveNetworkInfo.implementation = function() {\n"
        "      return createFakeNetInfo();\n"
        "    };\n"
        "\n"
        "    TelephonyManager.isDataEnabled.implementation = function() {\n"
        "      return netType === 0;\n"
        "    };\n"
        "\n"
        "    TelephonyManager.getDataState.implementation = function() {\n"
        "      return netType === 0 ? 2 : this.getDataState();\n"
        "    };\n"
        "\n"
        "    ConnectivityManager.getAllNetworks.implementation = function() {\n"
        "      const active = this.getActiveNetwork();\n"
        "      return active != null ? Java.array('android.net.Network', [active]) : this.getAllNetworks();\n"
        "    };\n"
        "\n"
        "    ConnectivityManager.getNetworkCapabilities.implementation = function(network) {\n"
        "      const builder = Builder.$new();\n"
        "      if (netType === 0) {\n"
        "        builder.addTransportType(TRANSPORT_CELLULAR);\n"
        "        builder.removeTransportType(TRANSPORT_WIFI);\n"
        "      } else {\n"
        "        builder.addTransportType(TRANSPORT_WIFI);\n"
        "        builder.removeTransportType(TRANSPORT_CELLULAR);\n"
        "      }\n"
        "      builder.removeTransportType(TRANSPORT_VPN);\n"
        "      builder.addCapability(CAP_INTERNET);\n"
        "      builder.addCapability(CAP_NOT_VPN);\n"
        "      builder.addCapability(CAP_VALIDATED);\n"
        "      return builder.build();\n"
        "    };\n"
    );
    #endif

    #if defined(NETWORK_MOBILE_DATA_LOCAL_IP) || defined(NETWORK_WIFI_LOCAL_IP)
    g_string_append(js,
        "    const NetworkInterface = Java.use('java.net.NetworkInterface');\n"
        "    const InetAddress = Java.use('java.net.InetAddress');\n"
        "    const Vector = Java.use('java.util.Vector');\n"
        "\n"
    );

    #ifdef NETWORK_MOBILE_DATA_LOCAL_IP
    g_string_append_printf(js, "    const mobileIp = '%s';\n", NETWORK_MOBILE_DATA_LOCAL_IP);
    #else
    g_string_append(js, "    const mobileIp = null;\n");
    #endif

    #ifdef NETWORK_WIFI_LOCAL_IP
    g_string_append_printf(js, "    const wifiIp = '%s';\n", NETWORK_WIFI_LOCAL_IP);
    #else
    g_string_append(js, "    const wifiIp = null;\n");
    #endif

    #ifdef NETWORK_NET_TYPE
    g_string_append(js, "    const isMobileActive = (netType === 0);\n");
    g_string_append(js, "    const isWifiActive = (netType === 1);\n");
    #else
    g_string_append(js, "    const isMobileActive = false;\n");
    g_string_append(js, "    const isWifiActive = false;\n");
    #endif

    g_string_append(js,
        "    const createInterface = function(name, id, ip, hasIp) {\n"
        "      return hasIp ? NetworkInterface.$new(name, id, [InetAddress.getByName(ip)]) : NetworkInterface.$new(name, id, []);\n"
        "    };\n"
        "\n"
        "    NetworkInterface.getNetworkInterfaces.implementation = function() {\n"
        "      const realEnum = this.getNetworkInterfaces();\n"
        "      const list = Vector.$new();\n"
        "      let mobileFound = false, wifiFound = false;\n"
        "\n"
        "      while (realEnum.hasMoreElements()) {\n"
        "        const iface = Java.cast(realEnum.nextElement(), NetworkInterface);\n"
        "        const name = iface.getName();\n"
        "\n"
        "        if (name && (name.indexOf('tun') < 0 && name.indexOf('ppp') < 0)) {\n"
        "          if (name.startsWith('rmnet_data0')) {\n"
        "            mobileFound = true;\n"
        "            if (mobileIp !== null) list.add(createInterface(name, 100, mobileIp, isMobileActive));\n"
        "            else list.add(iface);\n"
        "          } else if (name.startsWith('wlan0')) {\n"
        "            wifiFound = true;\n"
        "            if (wifiIp !== null) list.add(createInterface(name, 102, wifiIp, isWifiActive));\n"
        "            else list.add(iface);\n"
        "          } else {\n"
        "            list.add(iface);\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "\n"
        "      if (!mobileFound && mobileIp !== null) list.add(createInterface('rmnet_data0', 100, mobileIp, isMobileActive));\n"
        "      if (!wifiFound && wifiIp !== null) list.add(createInterface('wlan0', 102, wifiIp, isWifiActive));\n"
        "      return list.elements();\n"
        "    };\n"
    );
    #endif

    g_string_append(js,
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking network/VPN: ' + e);\n"
        "  }\n"
        "\n"
    );
    #endif

    // ============================================
    // Close Java.perform
    // ============================================
    g_string_append(js,
        "});\n"
    );

    // Compile and run script
    puppy_script = gum_script_backend_create_sync(backend, "puppy_js", js->str, NULL, cancellable, &error);
    g_string_free(js, TRUE);

    if (error != NULL) {
        LOGE("PuppyJS script compilation failed: %s", error->message);
        g_error_free(error);
        return;
    }

    gum_script_set_message_handler(puppy_script, on_message, NULL, NULL);
    gum_script_load_sync(puppy_script, cancellable);

    LOGI("Woof! Display and RAM Java hooks successfully deployed via GumJS.");

    // Keep the thread alive to process GumJS background events
    GMainContext *context = g_main_context_get_thread_default();
    while (TRUE) {
        if (g_main_context_pending(context)) {
            g_main_context_iteration(context, TRUE);
        } else {
            usleep(100000); // 100ms sleep
        }
    }
}