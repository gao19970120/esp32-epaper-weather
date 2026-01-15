import urllib.request
import urllib.parse
import json
import gzip
import io

def check_weather():
    base_url = "https://mn7fc2gvv8.re.qweatherapi.com/v7/weather/now"
    params = {
        "location": "116.9927215576172,32.601150527048645",
        "key": "d359a2923e47450583ae692917ea4202",
        "lang": "zh",
        "unit": "m"
    }
    
    url = f"{base_url}?{urllib.parse.urlencode(params)}"
    
    try:
        print(f"Requesting {url}")
        # 创建请求，模拟浏览器的 User-Agent 以防万一
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        
        with urllib.request.urlopen(req) as response:
            print(f"Status Code: {response.status}")
            content = response.read()
            
            # 尝试解压
            try:
                if content.startswith(b'\x1f\x8b'):
                    print("Detected GZIP content, decompressing...")
                    text = gzip.decompress(content).decode('utf-8')
                else:
                    text = content.decode('utf-8')
            except Exception as e:
                print(f"Decompression/Decode failed: {e}")
                text = content.decode('utf-8', errors='ignore')

            print("\nResponse Body:")
            try:
                data = json.loads(text)
                print(json.dumps(data, indent=2, ensure_ascii=False))
                
                if 'now' in data:
                    print(f"\n[RESULT] Current Icon Code: {data['now']['icon']}")
                    print(f"[RESULT] Current Text: {data['now']['text']}")
                else:
                    print("\n[WARN] 'now' field not found in response")
            except json.JSONDecodeError:
                print(text)
                
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    check_weather()
