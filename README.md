# Webcam Perspective Correction

C++17 と OpenCV 4 を使用し、Windows のWebカメラ映像を低遅延で台形補正するアプリケーションです。左側に入力映像と4つの編集点、右側に `cv::warpPerspective` の結果を表示します。

補正済み映像は次の方法で外部利用できます。

- **Spout2（推奨）**: D3D11共有テクスチャとして `Perspective Camera` Senderを公開。OBSでの利用に適します。
- **Media Foundation仮想カメラ（フォールバック）**: 一般的なカメラ入力に対応するアプリ向けです。
- **H.264 MP4録画**: Media Foundation Sink Writerを専用スレッドで実行します。

現在の台形補正はOpenCV CPU版です。Spout出力は補正済みBGRフレームを再利用BGRAバッファへ変換し、D3D11共有テクスチャへアップロードします。将来、補正処理をD3D11シェーダーへ置き換えた場合は `SpoutOutput` の `SendTexture` 経路へ差し替えられる構成です。

## 必要環境

- Windows 11 build 22000以降（仮想カメラを使用する場合）
- Visual Studio 2022/2026（Desktop development with C++）
- CMake 3.21 以降
- OpenCV 4.x（`core`, `imgproc`, `highgui`, `videoio`）
- Spout2 2.007.017（CMake構成時に公式GitHubから自動取得、BSD-2-Clause）
- OBSでSpout入力を使う場合: OBS Spout2 Plugin 1.12.0以降

## ビルド

OpenCV の CMake パッケージを指定して構成します。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOpenCV_DIR="C:/opencv/build"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Visual Studio 2026と既存のvcpkgを使う例です。

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

OpenCV を vcpkg で導入済みの場合は、環境に合わせた toolchain を指定できます。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

Spout2は初回のCMake構成時に `build/_deps` へ自動取得され、`SpoutDX_static` として静的リンクされます。バージョンと配布アーカイブのSHA-256を固定しているため、意図しない更新は行われません。アプリ実行時にSpout SDK DLLは不要です。ライセンスは `licenses/Spout2-LICENSE.txt` に収録しています。

初回構成にはGitHubへのネットワーク接続が必要です。オフライン環境では、別途用意したSpout2ソースのルートを指定できます。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOpenCV_DIR="C:/opencv/build" `
  -DFETCHCONTENT_SOURCE_DIR_SPOUT2="C:/SDK/Spout2"
```

OpenCV のDLLは検索パス上に必要です。Visual Studioから実行する場合、作業ディレクトリは自動的にソースルートへ設定されます。

ビルド後の主な成果物は次のとおりです。

```text
build/Release/WebcamPerspectiveCorrection.exe
build/Release/PerspectiveCameraMediaSource.dll
build/Release/PerspectiveCameraMediaSourceSmokeTest.exe
build/Release/VideoOutputsSmokeTest.exe
```

## 操作

- 左側の点をマウスドラッグ: 補正点を移動
- `R`: 4点を10%/90%の初期位置へ戻す
- `Space`: 補正のON/OFF
- `C`: インデックス0～7から次に開けるカメラへ切り替え
- `M`: 選択中カメラの次のネイティブモード（解像度/FPS/ピクセル形式）へ切り替え
- `O`: 外部出力方式を Spout / VirtualCamera の順に切り替え
- `S`: 選択中の外部出力を開始/停止
- `G`: H.264 MP4録画を開始/停止
- `V`: 仮想カメラを選択して開始/停止（互換ショートカット）
- `Esc` またはウィンドウを閉じる: 終了して設定を保存

画面上部の `Start/Stop Output`、`Method`、`Start/Stop Recording`、`Next Camera`、`Next Native Mode` ボタンも使用できます。現在の入力デバイス、キャプチャ解像度、FPS、ピクセル形式、処理FPS、外部出力状態、録画状態を常時表示します。

点の順序は、1: 左上、2: 右上、3: 右下、4: 左下です。点が交差するなど無効な四角形になった場合は赤色で表示し、最後に有効だった変換行列を継続使用します。

## 設定

`config/settings.json` でカメラ、要求解像度/FPS/ピクセル形式、出力方法、出力サイズ、録画先、正規化された4点を指定できます。既定出力は1920×1080/30fpsです。`output.follows_capture` を `1` にすると出力サイズを実際のカメラ取得サイズへ追従させられます。第1コマンドライン引数で別の設定ファイルも指定できます。

```json
{
    "camera": {
        "index": 0,
        "width": 1920,
        "height": 1080,
        "fps": 30.0,
        "backend": "MSMF",
        "pixel_format": "MJPG"
    },
    "output": {
        "width": 1920,
        "height": 1080,
        "fps": 30.0,
        "follows_capture": 0,
        "method": "Spout",
        "recording_directory": "recordings"
    }
}
```

相対指定の録画先はWindowsの「ビデオ」フォルダを基準に解決されます。既定では `%USERPROFILE%\Videos\recordings` に `perspective_YYYYMMDD_HHMMSS.mp4` を作成します。絶対パスも指定できます。

Media Foundationが列挙したデバイスと全ネイティブモードを表示する場合:

```powershell
.\build\Release\WebcamPerspectiveCorrection.exe --list-cameras
```

カメラ0で1920×1080/30fpsに最も近いネイティブモードを実際に開始し、取得フレーム寸法を検証する場合:

```powershell
.\build\Release\WebcamPerspectiveCorrection.exe --probe-camera 0
```

起動中の仮想カメラを別プロセスのMedia Foundationクライアントとして開き、映像サンプルまで検証する場合（表示された仮想カメラのインデックスを指定）:

```powershell
.\build\Release\WebcamPerspectiveCorrection.exe --probe-virtual-camera 1
```

仮想カメラを一時的に登録・列挙して停止する統合試験（管理者として起動したPowerShellから実行します）:

```powershell
.\build\Release\WebcamPerspectiveCorrection.exe --test-virtual-camera
```

既定ではMedia Foundationで物理カメラのネイティブモードを列挙し、1920×1080/30fpsに最も近いモードを選択します。開けない場合はDirectShow、OpenCV自動選択の順にフォールバックします。`backend` は `DSHOW`、`MSMF`、`ANY` から選べます。選択モードと実際の取得サイズは画面上部に別々に表示されます。

## OBS / Spout2

同一PC上のOBSへ出力する場合はSpout2を推奨します。仮想カメラと異なり、カメラドライバー形式への再変換やMedia Foundation Frame Serverを経由せず、D3D11共有テクスチャをOBS側から直接参照できます。

1. [OBS Spout2 Plugin](https://github.com/Off-World-Live/obs-spout2-plugin/releases) の64-bit版をOBSへ導入します。
2. 本アプリの `Method` を `Spout` にします。
3. `Start Output` または `S` キーで送信を開始します。
4. OBSでソースを追加し、`Spout2 Capture` を選択します。
5. Senderで `Perspective Camera` を選択します。

ノートPCなどGPUが複数ある環境では、本アプリとOBSを同じグラフィックスアダプターで実行してください。異なるアダプター間ではD3D11共有テクスチャを開けない場合があります。

## H.264録画

`Start Recording` または `G` キーで補正済み映像の録画を開始します。Media FoundationのH.264エンコーダーを使用し、利用可能な場合はハードウェア変換を有効にします。録画処理は専用スレッドで行い、処理が追いつかない場合は未処理フレームを蓄積せず最新フレームを優先します。

録画停止時またはアプリ終了時にSink Writerを `Finalize` してMP4を確定します。録画中にプロセスを強制終了すると、MP4が再生できない場合があります。

## 仮想カメラ（フォールバック）

`PerspectiveCameraMediaSource.dll` は `IMFActivate`、`IMFMediaSourceEx`、`IMFMediaStream2` を実装するCOM Media Sourceです。Windows Camera Frame Serverから読み込めるよう、アプリはDLLを `HKLM\Software\Classes\CLSID` へ登録し、`MFCreateVirtualCamera` でCurrentUserアクセス・Session寿命の `Perspective Camera Windows Virtual Camera` を作成します。停止またはアプリ終了時に仮想カメラは停止・解放されます。

通常のプレビュー、Spout出力、録画は標準ユーザー権限で使用できます。仮想カメラを開始する場合は、アプリを右クリックして「管理者として実行」してください。管理者権限はCOM Media SourceのHKLM登録に必要です。

補正済みBGRフレームは出力FPSで間引いてI420へ変換し、再利用バッファからNV12の最新2スロット共有メモリへ書き込みます。Media Sourceは外部アプリからサンプル要求が来た時だけ最新スロットをMFバッファへコピーします。入力が遅い場合は最新フレームを再利用し、切断時は最後のフレーム、起動前は黒フレームを返します。フレームFIFOは使用しません。

COM登録だけを解除する場合は、仮想カメラを使用しているアプリをすべて閉じた後、管理者として起動したPowerShellから次を実行します。この操作はHKLMのMedia Source登録を削除します。

```powershell
regsvr32 /u ".\build\Release\PerspectiveCameraMediaSource.dll"
```

Media Foundation仮想カメラは外部ソフトやWindows Frame Serverとの組み合わせにより互換性差があります。DirectShow経路で利用できてもMedia Foundation経路で最初のサンプル取得に失敗する環境があるため、OBS用途ではSpoutを優先してください。

## アーキテクチャ

```text
Camera Thread
    ↓ latest frame only
FrameProcessor Thread
    ↓ cv::getPerspectiveTransform (points changed only)
    ↓ cv::warpPerspective
    ├─→ UI Preview
    └─→ OutputManager Thread (latest frame only)
            ├─→ SpoutOutput → D3D11 shared texture → OBS
            ├─→ VirtualCameraOutput → NV12 shared memory → MF Media Source
            └─→ RecordingOutput Thread → Media Foundation H.264 MP4
```

`IVideoOutput` が外部出力の `configure` / `start` / `submitFrame` / `stop` を抽象化し、`SpoutOutput`、`VirtualCameraOutput`、`RecordingOutput` が実装します。カメラ取得、射影変換、UI、外部出力、録画エンコードは相互にブロックしない構成です。

## 低遅延設計

- カメラ取得、射影変換、UIを別スレッドに分離
- キューを持たず常に最新1フレームだけを保持
- スレッド間は `std::swap` で `cv::Mat` のヘッダー所有権を交換
- 点変更時だけ `cv::getPerspectiveTransform` を実行
- 出力 `cv::Mat` は同じサイズ・型なら OpenCV が既存領域を再利用
- `CAP_PROP_BUFFERSIZE=1` を要求（バックエンドが未対応の場合は無視されます）
- 外部出力が停止中の場合、OutputManager用のフレームコピーを省略
- SpoutのBGRA変換バッファとD3D11 Senderリソースを再利用
- 録画は専用の最新フレームバッファを使用し、エンコード遅延をカメラ処理へ波及させない
- 仮想カメラ出力FPSより速い入力フレームはNV12変換前に破棄
- プロセス間共有は2スロットのシーケンス検証方式で、FIFOキューなし

1080p/60fpsの達成可否はカメラの出力形式、USB帯域、CPU、OpenCVビルドの最適化に依存します。性能確認は必ず `Release` 構成で行ってください。画面左上に実測の処理FPSを表示します。

## テスト

```powershell
ctest --test-dir build -C Release --output-on-failure
```

- `PerspectiveCameraMediaSourceSmokeTest`: COM Media Sourceの起動、提供アロケータ、サンプル取得、再アクティベーションを検証
- `VideoOutputsSmokeTest`: Spout SenderからReceiverへのフレーム受信と、H.264 MP4の生成・コーデック再読込を検証

手動確認ではRelease版を起動し、画面上の `Process` が目標FPSを維持していることを確認してください。OBSでは `Spout2 Capture` のSender一覧とプレビュー映像の両方を確認します。
