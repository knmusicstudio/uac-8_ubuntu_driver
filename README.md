# ZOOM UAC-8 Linux Bridge (JACK + libusb)

[English](#english) | [日本語](#japanese)

---

<a name="english"></a>
## English

An experimental user-space audio bridge for the **ZOOM UAC-8** audio interface on Linux via JACK Audio Connection Kit and libusb.

### ⚠️ Disclaimer
* This project is **unofficial and experimental**.
* It is **NOT** affiliated with, endorsed by, or supported by ZOOM Corporation.
* Use this software **at your own risk**. The author assumes no responsibility for any hardware damage, audio spikes, hearing damage, or data loss.

### Prerequisites (Ubuntu / Debian)
Install the required build dependencies:

```bash
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev libjack-jackd2-dev libusb-1.0-0-dev
```
Make sure your user belongs to the audio group and has appropriate permissions to access USB devices.
How to Build
```bash
git clone https://github.com/knmusicstudio/uac-8_ubuntu_driver/zoom-uac8-linux-bridg.git
cd zoom-uac8-linux-bridge
make
```
How to Run
Start the JACK server using the dummy driver (e.g., via QjackCtl).

Connect your ZOOM UAC-8 to your PC via USB.

Launch the bridge:

```Bash
./uac8_bridge
Click "起動" (Start) on the GUI window.
```
In QjackCtl / Patchbay, route audio to uac8_jack:playback_1 through playback_18.

License
MIT License

日本語
JACK Audio Connection Kit と libusb を使用して、Linux 上で ZOOM UAC-8 オーディオインターフェースを動作させるための実験的なユーザ空間オーディオブリッジです。

⚠️ 免責事項
本プロジェクトは非公式かつ実験的なものです。

株式会社ズーム（ZOOM Corporation）とは一切関係がなく、公式なサポートや推奨を受けたものではありません。

本ソフトウェアの使用は完全自己責任でお願いいたします。機器の故障、突発的な大音量による聴覚・機材への被害、データの損失等に関して、作者は一切の責任を負いません。

必要パッケージの導入 (Ubuntu / Debian 系)
ビルドに必要な依存パッケージをインストールしてください
```Bash
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev libjack-jackd2-dev libusb-1.0-0-dev
```
※ お使いのユーザーが audio グループに所属し、USB デバイスへのアクセス権限を持っていることを確認してください。

ビルド手順
```Bash
git clone https://github.com/knmusicstudio/zoom-uac8-linux-bridge.git
cd zoom-uac8-linux-bridge
make
```
使い方
QjackCtl 等をdummyで起動し、JACK サーバーを開始します。

ZOOM UAC-8 を PC に USB 接続します。

プログラムを実行します：

```Bash
./uac8_bridge
```
表示されたウィンドウの 「起動」 ボタンを押します。

QjackCtl のパッチベイや接続画面で、uac8_jack:playback_1 〜 playback_18 に音声をルーティングします。

ライセンス
MIT License
