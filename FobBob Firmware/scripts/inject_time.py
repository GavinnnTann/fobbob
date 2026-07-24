import time
Import("env")
# Inject current Unix timestamp so firmware can seed the RTC at flash time
env.Append(CPPDEFINES=[("BUILD_TIMESTAMP", int(time.time()))])
print(f"[inject_time] BUILD_TIMESTAMP = {int(time.time())}")
