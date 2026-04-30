import sys

with open(sys.argv[1], 'rb') as f:
    data = f.read()

# Remove the VSCode metadata pattern
METADATA = b'{"$mid":24,"mimeType":"cache_control","data":"ZXBoZW1lcmFs"}'
data = data.replace(METADATA, b'')
# Fix any double-brace corruption
data = data.replace(b'{{', b'{')
data = data.replace(b'}}', b'}')

with open(sys.argv[1], 'wb') as f:
    f.write(data)

print(f'Cleaned {sys.argv[1]}: {len(data)} bytes')
