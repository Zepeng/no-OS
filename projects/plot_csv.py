import pandas as pd
import matplotlib.pyplot as plt

with open('test.txt', 'r') as f:
    lines = f.readlines()

timestamps = []
voltages = []

for line in lines:
    timestamp_str, voltage_str = line.strip().split('] ')
    timestamp_str = timestamp_str.strip('[')
    voltage_values = [float(v.strip()) for v in voltage_str.split(',')]
    
    timestamps.append(pd.to_datetime(timestamp_str))
    voltages.append(voltage_values)


df = pd.DataFrame(voltages, columns=['Ch0', 'Ch1', 'Ch2', 'Ch3'])
df['timestamp'] = timestamps
df['elapsed_sec'] = (df['timestamp'] - df['timestamp'].iloc[0]).dt.total_seconds()

df_subset = df[(df['elapsed_sec'] >= 3) & (df['elapsed_sec'] <= 4)]


plt.figure(figsize=(10, 5))
plt.plot(df_subset['elapsed_sec'], df_subset['Ch2'], label='Channel 2 Voltage')
plt.xlabel('Elapsed Time (seconds)')
plt.ylabel('Voltage (V)')
plt.title('Channel 2 Voltage vs. Time (First 1 Second)')
plt.grid(True)
plt.legend()
plt.tight_layout()
plt.show()