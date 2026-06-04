# 2026-06-04T12:06:54.835758900
import vitis

client = vitis.create_client()
client.set_workspace(path="SPO2_monitor")

comp = client.get_component(name="SpO2_app")
status = comp.clean()

status = comp.clean()

platform = client.get_component(name="platform")
status = platform.build()

comp.build()

vitis.dispose()

