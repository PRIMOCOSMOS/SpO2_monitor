# 2026-06-04T17:34:28.139912800
import vitis

client = vitis.create_client()
client.set_workspace(path="SPO2_monitor")

comp = client.get_component(name="SpO2_app")
status = comp.clean()

platform = client.get_component(name="platform")
status = platform.build()

comp.build()

vitis.dispose()

