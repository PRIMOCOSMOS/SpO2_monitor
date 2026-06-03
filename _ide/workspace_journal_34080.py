# 2026-05-30T21:51:40.969535300
import vitis

client = vitis.create_client()
client.set_workspace(path="SPO2_monitor")

platform = client.get_component(name="platform")
status = platform.build()

comp = client.get_component(name="SpO2_app")
comp.build()

domain = platform.get_domain(name="freertos_psu_cortexa53_0")

status = domain.set_config(option = "os", param = "freertos_total_heap_size", value = "2097152")

status = platform.build()

comp.build()

status = platform.build()

comp.build()

