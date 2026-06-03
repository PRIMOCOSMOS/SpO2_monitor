# 2026-05-30T16:36:37.825438
import vitis

client = vitis.create_client()
client.set_workspace(path="SPO2_monitor")

platform = client.create_platform_component(name = "platform",hw_design = "$COMPONENT_LOCATION/../SPO2_monitor_final.xsa",os = "freertos",cpu = "psu_cortexa53_0",domain_name = "freertos_psu_cortexa53_0",architecture = "64-bit")

platform = client.get_component(name="platform")
status = platform.build()

comp = client.create_app_component(name="SpO2_app",platform = "$COMPONENT_LOCATION/../platform/export/platform/platform.xpfm",domain = "freertos_psu_cortexa53_0")

vitis.dispose()

