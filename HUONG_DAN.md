# Hướng dẫn cài đặt và chạy thử ltDriver

Tài liệu này hướng dẫn từng bước để **cài đặt**, **build** và **test** toàn bộ dự án trên một máy Linux bất kỳ (Ubuntu/Debian khuyến nghị) sau khi copy source vào.

---

## Yêu cầu hệ thống

| Thành phần | Yêu cầu |
|---|---|
| Hệ điều hành | Ubuntu 20.04 / 22.04 / Debian 11+ (hoặc tương đương) |
| Quyền | Có `sudo` |
| Kiến trúc | x86_64 hoặc x86 32-bit |
| Kernel | 4.x trở lên (có hỗ trợ DES crypto) |

> **WSL2:** Chỉ build được app (`ltfm`), **không** build được kernel module vì WSL2 không có kernel headers mặc định. Dùng máy Linux thật hoặc VM.

---

## Bước 1 — Cài đặt phần mềm cần thiết

```bash
# Cập nhật package list
sudo apt-get update

# Cài compiler, make, và kernel headers của kernel đang chạy
sudo apt-get install -y build-essential linux-headers-$(uname -r)

# Kiểm tra kernel headers đã có chưa
ls /lib/modules/$(uname -r)/build
# Nếu thư mục tồn tại → OK
```

---

## Bước 2 — Kiểm tra kernel có hỗ trợ DES không

```bash
grep CONFIG_CRYPTO_DES /boot/config-$(uname -r)
```

Kết quả cần thấy một trong hai:
- `CONFIG_CRYPTO_DES=y` → DES đã tích hợp vào kernel, không cần làm gì thêm
- `CONFIG_CRYPTO_DES=m` → DES là module, cần load thủ công trước (xem Bước 4)

Nếu không có dòng nào → kernel không hỗ trợ DES, cần dùng kernel khác.

---

## Bước 3 — Build dự án

```bash
# Vào thư mục gốc của project
cd ltDriver

# Build tất cả: 2 kernel module + app ltfm
make all
```

Sau khi thành công, kiểm tra:

```bash
ls driver/ltcrypt/ltcrypt.ko       # kernel module mã hóa DES
ls driver/usbmouse/ltusbmouse.ko   # kernel module USB mouse
ls app/ltfm                        # ứng dụng file manager
```

Nếu chỉ muốn build app (không cần kernel headers):
```bash
make app
```

---

## Bước 4 — Load driver mã hóa DES

```bash
# Nếu CONFIG_CRYPTO_DES=m, cần load module DES trước
sudo modprobe des_generic

# Load driver ltcrypt
sudo insmod driver/ltcrypt/ltcrypt.ko

# Kiểm tra load thành công
dmesg | tail -5
# Phải thấy dòng: "ltcrypt: loaded, major=XXX"

# Kiểm tra device đã được tạo chưa
ls -la /dev/ltcrypt
```

Nếu `/dev/ltcrypt` **chưa xuất hiện** (udev chưa tạo), tạo thủ công:

```bash
make mknod
# Hoặc thủ công:
MAJOR=$(awk '$2=="ltcrypt"{print $1}' /proc/devices)
sudo mknod /dev/ltcrypt c $MAJOR 0
sudo chmod 666 /dev/ltcrypt
```

---

## Bước 5 — Test ứng dụng ltfm (mã hóa / giải mã file)

```bash
# Chạy ứng dụng
./app/ltfm
```

Giao diện menu hiện ra:

```
=== ltfm — File Manager with DES Encryption ===
Device: /dev/ltcrypt

Menu:
  1. List directory
  2. Encrypt file
  3. Decrypt file
  4. View encrypted file (hex)
  5. Set encryption key
  0. Quit
```

### Test nhanh từng bước:

**1. Tạo file thử:**
```bash
echo "Day la noi dung bi mat can ma hoa!" > /tmp/test.txt
cat /tmp/test.txt
```

**2. Trong menu ltfm:**
- Chọn `5` → nhập khóa 8 ký tự, ví dụ: `testkey1` (Enter để xác nhận, ký tự sẽ ẩn)
- Chọn `2` → nhập đường dẫn `/tmp/test.txt` → tạo ra `/tmp/test.txt.enc`
- Chọn `4` → nhập `/tmp/test.txt.enc` → xem hex dump nội dung đã mã hóa
- Chọn `3` → nhập `/tmp/test.txt.enc` → tạo ra `/tmp/test.txt` (file gốc phục hồi)
- Chọn `0` → thoát

**3. So sánh kết quả ngoài terminal:**
```bash
diff /tmp/test.txt <(cat /tmp/test.txt)
# Không có output → hai file giống nhau → mã hóa/giải mã thành công
```

---

## Bước 6 — Test driver USB Mouse (tùy chọn)

> Cần có chuột USB vật lý cắm vào máy.

```bash
# Load driver
sudo insmod driver/usbmouse/ltusbmouse.ko

# Xem cấu trúc bus USB để tìm chuột (Driver=usbhid)
lsusb -t
# Ví dụ:
# /:  Bus 02.Port 1: Dev 1, Class=root_hub, ...
#     |__ Port 1: Dev 2, If 0, Class=Human Interface Device, Driver=usbhid, 12M
#
# Khi đó ID đầy đủ sẽ là: 2-1:1.0

# Unbind chuột khỏi driver usbhid gốc (thay 2-1:1.0 bằng ID thực tế của bạn)
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/usbhid/unbind

# Bind vào driver ltusbmouse
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/ltusbmouse/bind

# Theo dõi log realtime, di chuyển / nhấp chuột để thấy sự kiện
sudo dmesg -w
# Kỳ vọng: log dạng "ltusbmouse: btn=00 X=5 Y=-3"
```

---

## Tắt / gỡ driver

```bash
# Gỡ tất cả driver (cả hai module)
make unload

# Hoặc gỡ từng cái:
sudo rmmod ltusbmouse
sudo rmmod ltcrypt
```

---

## Xử lý lỗi thường gặp

| Lỗi | Nguyên nhân | Cách sửa |
|---|---|---|
| `No such file or directory: /lib/modules/.../build` | Chưa cài kernel headers | `sudo apt-get install linux-headers-$(uname -r)` |
| `ERROR: could not insert 'ltcrypt': Unknown symbol in module` | Kernel không có DES | `sudo modprobe des_generic` hoặc kiểm tra `CONFIG_CRYPTO_DES` |
| `/dev/ltcrypt: No such file or directory` | udev chưa tạo node | `make mknod` |
| `ioctl SET_KEY: Operation not permitted` | Chạy ltfm không có quyền đọc `/dev/ltcrypt` | `sudo chmod 666 /dev/ltcrypt` |
| `ltcrypt: failed to allocate DES cipher` | DES module chưa được load | `sudo modprobe des_generic` |
| Chuột không có log sau khi bind | `ltusbmouse` chưa claim được device | Kiểm tra lại ID trong `/sys/bus/usb/drivers/usbhid/` |

---

## Dọn dẹp build

```bash
make clean
# Xóa toàn bộ file .ko, .o, .mod.c và binary ltfm
```
