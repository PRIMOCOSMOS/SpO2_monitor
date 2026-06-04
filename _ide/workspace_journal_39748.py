# 2026-06-04T14:55:36.359048100
import vitis

client = vitis.create_client()
client.set_workspace(path="SPO2_monitor")

comp = client.get_component(name="SpO2_app")
status = comp.clean()

platform = client.get_component(name="platform")
status = platform.build()

comp.build()

status = comp.clean()

status = platform.build()

comp.build()

status = comp.clean()

status = comp.clean()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = comp.clean()

status = comp.clean()

status = comp.clean()

status = platform.build()

comp.build()

status = comp.clean()

status = platform.build()

comp.build()

status = comp.clean()

status = platform.build()

comp.build()

status = comp.clean()

status = comp.clean()

status = platform.build()

comp.build()

status = comp.clean()

status = comp.clean()

status = comp.clean()

status = platform.build()

comp.build()

status = comp.clean()

status = comp.clean()

status = comp.clean()

status = platform.build()

status = comp.clean()

status = comp.clean()

comp.build()

status = comp.clean()

status = platform.build()

comp.build()

vitis.dispose()

vitis.dispose()

