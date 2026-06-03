# 2026-05-30T20:16:10.648713200
import vitis

client = vitis.create_client()
client.set_workspace(path="SPO2_monitor")

platform = client.get_component(name="platform")
status = platform.build()

comp = client.get_component(name="SpO2_app")
comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

vitis.dispose()

