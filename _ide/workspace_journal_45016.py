# 2026-06-04T18:02:46.124350200
import vitis

client = vitis.create_client()
client.set_workspace(path="SPO2_monitor")

platform = client.get_component(name="platform")
domain = platform.get_domain(name="freertos_psu_cortexa53_0")

status = domain.set_config(option = "os", param = "freertos_total_heap_size", value = "8388608")

comp = client.get_component(name="SpO2_app")
status = comp.clean()

status = platform.build()

comp.build()

vitis.dispose()

