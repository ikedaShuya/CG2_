#include "WinApp.h" 

#include <format>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <numbers>

#include <strsafe.h>
#include <DbgHelp.h>
#include <xaudio2.h>

#include "Math.h"
#include "Input.h"
#include "DirectXCommon.h"
#include "D3DResourceLeakChecker.h"

#pragma comment(lib,"dxguid.lib")
#pragma comment(lib,"dxcompiler.lib")
#pragma comment(lib,"xaudio2.lib")
#pragma comment(lib,"Dbghelp.lib")

#ifdef USE_IMGUI
#include "externals/imgui/imgui.h"
#include "externals/imgui/imgui_impl_dx12.h"
#include "externals/imgui/imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

using namespace Math;

// チャンクヘッダ
struct ChunkHeader
{
	char id[4]; // チャンク毎のID
	int32_t size; // チャンクサイズ
};

// RIFFヘッダチャンク
struct RiffHeader
{
	ChunkHeader chunk; // "RIFF"
	char type[4]; // "WAVE"
};

// FMTチャンク
struct FormatChunk
{
	ChunkHeader chunk; // "fmt"
	WAVEFORMATEX fmt; // 波形フォーマット
};

// 音声データ
struct SoundData
{
	// 波形フォーマット
	WAVEFORMATEX wfex;
	// バッファの先頭アドレス
	BYTE *pBuffer;
	// バッファのサイズ
	unsigned int bufferSize;
};

SoundData SoundLoadWave(const char *filename)
{

	//----ファイルオープン----

	// ファイル入力ストリームのインスタンス
	std::ifstream file;
	// .wavファイルをバイナリモードで開く
	file.open(filename, std::ios_base::binary);
	// ファイルオープン失敗を検出する
	assert(file.is_open());

	//----.wavデータを読み込み----

	// RIFFヘッダーの読み込み
	RiffHeader riff;
	file.read((char *)&riff, sizeof(riff));
	// ファイルがRIFFかチェック
	if (strncmp(riff.chunk.id, "RIFF", 4) != 0) {
		assert(0);
	}
	// タイプがWAVEかチェック
	if (strncmp(riff.type, "WAVE", 4) != 0) {
		assert(0);
	}

	// Formatチャンクの読み込み
	FormatChunk format = {};
	// チャンクヘッダーの確認
	file.read((char *)&format, sizeof(ChunkHeader));
	if (strncmp(format.chunk.id, "fmt ", 4) != 0) {
		assert(0);
	}

	// チャンク本体の読み込み
	assert(format.chunk.size <= sizeof(format.fmt));
	file.read((char *)&format.fmt, format.chunk.size);

	// Dataチャンクの読み込み
	ChunkHeader data;
	file.read((char *)&data, sizeof(data));
	// JUNKチャンクを検出した場合
	if (strncmp(data.id, "JUNK", 4) == 0) {
		// 読み取り位置をJUNKチャンクの終わりまで進める
		file.seekg(data.size, std::ios_base::cur);
		// 再読み込み
		file.read((char *)&data, sizeof(data));
	}

	if (strncmp(data.id, "data", 4) != 0) {
		assert(0);
	}

	// Dataチャンクのデータ部（波形データ）の読み込み
	char *pBuffer = new char[data.size];
	file.read(pBuffer, data.size);

	//----ファイルクローズ----

	// Waveファイルを閉じる
	file.close();

	//----読み込んだ音声データをreturn----

	// returnする為の音声データ
	SoundData soundData = {};

	soundData.wfex = format.fmt;
	soundData.pBuffer = reinterpret_cast<BYTE *>(pBuffer);
	soundData.bufferSize = data.size;

	return soundData;
}

// 音声データを解放
void SoundUnload(SoundData *soundData)
{
	// バッファのメモリを解放
	delete[] soundData->pBuffer;

	soundData->pBuffer = 0;
	soundData->bufferSize = 0;
	soundData->wfex = {};
}

// 音楽再生
void SoundPlayWave(IXAudio2 *xAudio2, const SoundData &soundData) {

	HRESULT result;

	// 波形フォーマットを元にSourceVoiceの生成
	IXAudio2SourceVoice *pSourceVoice = nullptr;
	result = xAudio2->CreateSourceVoice(&pSourceVoice, &soundData.wfex);
	assert(SUCCEEDED(result));

	// 再生する波形データの設定
	XAUDIO2_BUFFER buf {};
	buf.pAudioData = soundData.pBuffer;
	buf.AudioBytes = soundData.bufferSize;
	buf.Flags = XAUDIO2_END_OF_STREAM;

	// 波形データの再生
	result = pSourceVoice->SubmitSourceBuffer(&buf);
	result = pSourceVoice->Start();
}

#pragma region math

struct Transform
{
	Vector3 scale;
	Vector3 rotate;
	Vector3 translate;
};

struct VertexData
{
	Vector4 position;
	Vector2 texcoord;
	Vector3 normal;
};

struct Material
{
	Vector4 color;
	int32_t enableLighting;
	float padding[3];
	Matrix4x4 uvTransform;
	float shininess;
};

struct TransformationMatrix
{
	Matrix4x4 WVP;
	Matrix4x4 World;
};

struct DirectionalLight
{
	Vector4 color; //!< ライトの色
	Vector3 direction; //!< ライトの向き
	float intensity; //!< 輝度
};

struct MaterialData
{
	std::string textureFilePath;
};

struct ModelData
{
	std::vector<VertexData> vertices;
	MaterialData material;
};

struct CameraForGPU
{
	Vector3 worldPosition;
};

#pragma endregion

#pragma region SystemBase

// ウィンドウプロシージャ
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {

#ifdef USE_IMGUI
	if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
		return true;
	}
#endif

	// メッセージに応じてゲーム固有の処理を行う
	switch (msg) {
		// ウィンドウが破棄された
		case WM_DESTROY:
		// osに対して、アプリの終了を伝える
		PostQuitMessage(0);
		return 0;
	}

	// 標準のメッセージ処理を行う
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

// Dump出力
static LONG WINAPI ExportDump(EXCEPTION_POINTERS *exception) {

	// 時刻を取得して、時刻を名前に入れたファイルを作成。Dumpsディレクトリ以下に出力
	SYSTEMTIME time;
	GetLocalTime(&time);
	wchar_t filePath[MAX_PATH] = { 0 };
	CreateDirectory(L"./Dumps", nullptr);
	StringCchPrintfW(filePath, MAX_PATH, L"./Dumps/%04d-%02d%02d-%02d%02d.dmp", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
	HANDLE dumpFileHandle = CreateFile(filePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);
	// processId（このexeのId）とクラッシュ（例外）の発生したthreadIdを取得
	DWORD processId = GetCurrentProcessId();
	DWORD threadId = GetCurrentThreadId();
	// 設定情報を入力
	MINIDUMP_EXCEPTION_INFORMATION minidumpInformation { 0 };
	minidumpInformation.ThreadId = threadId;
	minidumpInformation.ExceptionPointers = exception;
	minidumpInformation.ClientPointers = TRUE;
	// Dumpを出力。MiniDumpNormalは最低限の情報を出力するフラグ
	MiniDumpWriteDump(GetCurrentProcess(), processId, dumpFileHandle, MiniDumpNormal, &minidumpInformation, nullptr, nullptr);
	// 他に関連づけられているSEH例外ハンドラがあれば実行。通常はプロセスを終了する
	return EXCEPTION_EXECUTE_HANDLER;
}

#pragma endregion

#pragma region AssetLoading

MaterialData LoadMaterialTemplateFile(const std::string &directoryPath, const std::string &filename)
{
	//----中で必要となる変数の宣言----
	MaterialData materialData; // 構築するMaterialData
	std::string line; // ファイルから読んだ1行を格納するもの

	//----ファイルを開く----
	std::ifstream file(directoryPath + "/" + filename); // ファイルを開く
	assert(file.is_open()); // とりあえず開かなかったら止める

	//----ファイルを読み込んで、MaterialDataを構築する----
	while (std::getline(file, line)) {
		std::string identifier;
		std::istringstream s(line);
		s >> identifier;

		// identiffierに応じた処理
		if (identifier == "map_Kd") {
			std::string textureFilename;
			s >> textureFilename;
			// 連結してファイルパスにする
			materialData.textureFilePath = directoryPath + "/" + textureFilename;
		}
	}

	//----MaterialDataを返す----
	return materialData;
}

ModelData LoadObjFile(const std::string &directoryPath, const std::string &filename)
{
	//----必要な変数宣言----
	ModelData modelData; // 構築するModelData
	std::vector<Vector4> positions; // 位置
	std::vector<Vector3> normals; // 法線
	std::vector<Vector2> texcoords; // テクスチャ座標
	std::string line; // ファイルから読んだ1行を格納するもの

	//----ファイルを開く----
	std::ifstream file(directoryPath + "/" + filename); // ファイルを開く
	assert(file.is_open()); // とりあえず開けなかったら止める

	//----ファイルを読み込んで、ModelDataを構築していく----
	while (std::getline(file, line)) {
		std::string identifier;
		std::istringstream s(line);
		s >> identifier; // 先頭の識別子を読む

		// identifierに応じた処理
		if (identifier == "v") {
			Vector4 position;
			s >> position.x >> position.y >> position.z;
			position.x *= -1.0f;
			position.w = 1.0f;
			positions.push_back(position);
		} else if (identifier == "vt") {
			Vector2 texcoord;
			s >> texcoord.x >> texcoord.y;
			texcoord.y = 1.0f - texcoord.y;
			texcoords.push_back(texcoord);
		} else if (identifier == "vn") {
			Vector3 normal;
			s >> normal.x >> normal.y >> normal.z;
			normal.x *= -1.0f;
			normals.push_back(normal);
		} else if (identifier == "f") {

			VertexData triangle[3];
			// 面は三角形限定。その他は未対応
			for (int32_t faceVertex = 0; faceVertex < 3; ++faceVertex) {
				std::string vertexDefinition;
				s >> vertexDefinition;
				// 頂点の要素へのIndexは「位置/UV/法線」で格納されているので、分解してIndexを取得する
				std::istringstream v(vertexDefinition);
				uint32_t elementIndices[3];
				for (int32_t element = 0; element < 3; ++element) {
					std::string index;
					std::getline(v, index, '/'); // /区切りでインデックスを読んでいく
					elementIndices[element] = std::stoi(index);
				}
				// 要素へのIndexから、実際の要素の値を取得して、頂点を構築する
				Vector4 position = positions[elementIndices[0] - 1];
				Vector2 texcoord = texcoords[elementIndices[1] - 1];
				Vector3 normal = normals[elementIndices[2] - 1];
				triangle[faceVertex] = { position,texcoord,normal };
			}
			// 頂点を逆順で登録することで、回り順を逆にする
			modelData.vertices.push_back(triangle[2]);
			modelData.vertices.push_back(triangle[1]);
			modelData.vertices.push_back(triangle[0]);
		} else if (identifier == "mtllib") {
			// materialTemplateLibraryファイルの名前を取得する
			std::string materialFilename;
			s >> materialFilename;
			// 基本的にobjファイルと同一階層にmtlは存在させるので、ディレクトリ名とファイル名を渡す
			modelData.material = LoadMaterialTemplateFile(directoryPath, materialFilename);
		}
	}

	//----ModelDataを返す----
	return modelData;
}

#pragma endregion

// Windowsアプリでのエントリーポイント（main関数）
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {

	// 誰も補捉しなかった場合に(Unhandled)、補捉する関数を登録
	SetUnhandledExceptionFilter(ExportDump);

#pragma region ログファイル

	// ログのディレクトリを用意
	std::filesystem::create_directory("logs");

	// 現在時刻を取得（UTC時刻）
	std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	// ログファイルの名前にコンマ何秒はいらないので、削って秒にする
	std::chrono::time_point < std::chrono::system_clock, std::chrono::seconds>
		nowSeconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
	// 日本時間（PCの設定時間）に変換
	std::chrono::zoned_time localTime { std::chrono::current_zone(),nowSeconds };
	// formatを使って年月日_時分秒の文字列に変換
	std::string dateString = std::format("{:%Y%m%d_%H%M%S}", localTime);
	// 時刻を作ってファイル名を決定
	std::string logFilePath = std::string("logs/") + dateString + ".log";
	// ファイルを作って書き込み準備
	std::ofstream logStream(logFilePath);

#pragma endregion

#pragma region ウィンドウを作ろう

	// ポインタ
	WinApp *winApp = nullptr;

	// WindowsAPIの初期化
	winApp = new WinApp();
	winApp->Initialize();

#pragma endregion

	D3DResourceLeakChecker leakCheck;

	// ポインタ
	DirectXCommon *dxCommon = nullptr;

	// DirectXの初期化
	dxCommon = new DirectXCommon();
	dxCommon->Initialize(winApp);

#pragma region 音楽

	Microsoft::WRL::ComPtr<IXAudio2> xAudio2;
	IXAudio2MasteringVoice *masterVoice;

	// XAudioエンジンのインスタンスを生成
	HRESULT result = XAudio2Create(&xAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);

	// マスターボイスを生成
	result = xAudio2->CreateMasteringVoice(&masterVoice);

	// 音声読み込み
	SoundData soundData1 = SoundLoadWave("resources/audio/Alarm02.wav");

	// 音楽再生
	SoundPlayWave(xAudio2.Get(), soundData1);

#pragma endregion

#pragma region 入力デバイス

	// ポインタ
	Input *input = nullptr;

	// 入力の初期化
	input = new Input();
	input->Initialize(winApp);

#pragma endregion

#pragma region PSO

	HRESULT hr;

	//----RootSignatureを生成する----

	// RootSignature作成
	D3D12_ROOT_SIGNATURE_DESC descriptionRootSignature {};
	descriptionRootSignature.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	//----DescriptorRange----
	D3D12_DESCRIPTOR_RANGE descriptorRange[1] = {};
	descriptorRange[0].BaseShaderRegister = 0; // 0から始まる
	descriptorRange[0].NumDescriptors = 1; // 数は1つ
	descriptorRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // SRVを使う
	descriptorRange[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND; // Offsetを自動計算

	//----RootParameter----

	// RootParameter作成。PixelShaderのMaterialとVertexShaderのTransform
	D3D12_ROOT_PARAMETER rootParameters[5] = {};

	// [0] Material (PS, b0)
	rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // CBVを使う
	rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; // PixelShaderで使う
	rootParameters[0].Descriptor.ShaderRegister = 0; // レジスタ番号0とバインド

	// [1] Transform (VS, b0)
	rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // CBVを使う
	rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX; // VertexShaderで使う
	rootParameters[1].Descriptor.ShaderRegister = 0; // レジスタ番号0を使う

	// [2] Texture SRV (PS, t0)
	rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // DescriptorTableを使う
	rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; // PixelShaderで使う
	rootParameters[2].DescriptorTable.pDescriptorRanges = descriptorRange;
	rootParameters[2].DescriptorTable.NumDescriptorRanges = _countof(descriptorRange); // Tableで利用する数

	// [3] DirectionalLight (PS, b1)
	rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // CBVを使う
	rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; // PixelShaderで使う
	rootParameters[3].Descriptor.ShaderRegister = 1; // レジスタ番号1を使う

	// [4] Camera (PS, b2)
	rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[4].Descriptor.ShaderRegister = 2;

	descriptionRootSignature.pParameters = rootParameters; // ルートパラメータ配列へのポインタ
	descriptionRootSignature.NumParameters = _countof(rootParameters); // 配列の長さ

	//----Sampler----
	D3D12_STATIC_SAMPLER_DESC staticSamplers[1] = {};
	staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; // バイリニアフィルタ
	staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP; // 0~1の範囲外をリピート
	staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER; // 比較しない
	staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX; // ありったけのMipmapを使う
	staticSamplers[0].ShaderRegister = 0; // レジスタ番号0を使う
	staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; // PixelShaderで使う
	descriptionRootSignature.pStaticSamplers = staticSamplers;
	descriptionRootSignature.NumStaticSamplers = _countof(staticSamplers);

	// シリアライズしてバイナリにする
	Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob = nullptr;
	Microsoft::WRL::ComPtr<ID3DBlob> errorBlob = nullptr;
	hr = D3D12SerializeRootSignature(&descriptionRootSignature,
		D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
	if (FAILED(hr)) {
		//Log(logStream, reinterpret_cast<char*>(errorBlob->GetBufferPointer()));
		assert(false);
	}
	// バイナリを元に生成
	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature = nullptr;
	hr = dxCommon->GetDevice()->CreateRootSignature(0,
		signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
		IID_PPV_ARGS(&rootSignature));
	assert(SUCCEEDED(hr));

	//----InputLayoutの設定を行う----

	// InputLayout
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[3] = {};
	inputElementDescs[0].SemanticName = "POSITION";
	inputElementDescs[0].SemanticIndex = 0;
	inputElementDescs[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	inputElementDescs[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

	inputElementDescs[1].SemanticName = "TEXCOORD";
	inputElementDescs[1].SemanticIndex = 0;
	inputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
	inputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

	inputElementDescs[2].SemanticName = "NORMAL";
	inputElementDescs[2].SemanticIndex = 0;
	inputElementDescs[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

	D3D12_INPUT_LAYOUT_DESC inputLayoutDesc {};
	inputLayoutDesc.pInputElementDescs = inputElementDescs;
	inputLayoutDesc.NumElements = _countof(inputElementDescs);

	//----BlendStateの設定を行う----

	// BlendStateの設定
	D3D12_BLEND_DESC blendDesc {};
	// すべての色要素を書き込む
	blendDesc.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;

	//----RasterizerStateの設定を行う----

	// RasterzerStateの設定
	D3D12_RASTERIZER_DESC rasterizerDesc {};
	// 裏面（時計回り）を表示しない
	rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
	// 三角形の中を塗りつぶす
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;

	//----ShaderをCompileする----

	// Shaderをコンパイルする
	Microsoft::WRL::ComPtr<IDxcBlob> vertexShaderBlob = dxCommon->CompileShader(L"resources/shaders/Object3D.VS.hlsl", L"vs_6_0");
	assert(vertexShaderBlob != nullptr);

	Microsoft::WRL::ComPtr<IDxcBlob> pixelShaderBlob = dxCommon->CompileShader(L"resources/shaders/Object3D.PS.hlsl", L"ps_6_0");
	assert(pixelShaderBlob != nullptr);

	//----PSOを生成する----

	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPipelineStateDesc {};
	graphicsPipelineStateDesc.pRootSignature = rootSignature.Get(); // RootSignature
	graphicsPipelineStateDesc.InputLayout = inputLayoutDesc; // InputLayout
	graphicsPipelineStateDesc.VS = { vertexShaderBlob->GetBufferPointer(),
	vertexShaderBlob->GetBufferSize() }; // VertexShader
	graphicsPipelineStateDesc.PS = { pixelShaderBlob->GetBufferPointer(),
	pixelShaderBlob->GetBufferSize() }; // PixelShader
	graphicsPipelineStateDesc.BlendState = blendDesc; // BlenState
	graphicsPipelineStateDesc.RasterizerState = rasterizerDesc; // RasterizerState
	// 書き込むRTVの情報
	graphicsPipelineStateDesc.NumRenderTargets = 1;
	graphicsPipelineStateDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	// 利用するトポロジ（形状）のタイプ。三角形
	graphicsPipelineStateDesc.PrimitiveTopologyType =
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	// どのように画面に色を打ち込むかの設定（気にしなくて良い）
	graphicsPipelineStateDesc.SampleDesc.Count = 1;
	graphicsPipelineStateDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

	//----DepthStencilStateの設定----

	// DepthStencilStateの設定
	D3D12_DEPTH_STENCIL_DESC depthStencilDesc {};
	// Depthの機能を有効化する
	depthStencilDesc.DepthEnable = true;
	// 書き込みします
	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	// 比較関数はLessEqual。つまり、近ければ描画される
	depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	// DepthStencilの設定
	graphicsPipelineStateDesc.DepthStencilState = depthStencilDesc;
	graphicsPipelineStateDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

	// 実際に生成
	Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipelineState = nullptr;
	hr = dxCommon->GetDevice()->CreateGraphicsPipelineState(&graphicsPipelineStateDesc,
		IID_PPV_ARGS(&graphicsPipelineState));
	assert(SUCCEEDED(hr));

#pragma endregion

#pragma region Resources: 3D Object (ModelData)

	/**************************************************
	* 頂点データの作成とビュー
	**************************************************/

	// モデル読み込み
	ModelData modelData = LoadObjFile("resources/models/plane", "plane.obj");
	//----VertexResourceを生成する----
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource = dxCommon->CreateBufferResource(sizeof(VertexData) * modelData.vertices.size());

	//----VertexBufferViewを生成する----

	// 頂点バッファビューを作成する
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView {};
	// リソースの先頭のアドレスから使う
	vertexBufferView.BufferLocation = vertexResource->GetGPUVirtualAddress();
	// 使用するリソースのサイズは頂点6つ分のサイズ
	vertexBufferView.SizeInBytes = UINT(sizeof(VertexData) * modelData.vertices.size());
	// 1頂点あたりのサイズ
	vertexBufferView.StrideInBytes = sizeof(VertexData);

	//----Resourceにデータを書き込む----

	VertexData *vertexData = nullptr;
	vertexResource->Map(0, nullptr, reinterpret_cast<void **>(&vertexData)); // 書き込むためのアドレスを取得
	std::memcpy(vertexData, modelData.vertices.data(), sizeof(VertexData) * modelData.vertices.size()); // 頂点データをリソースにコピー

	/**************************************************
	* Transform
	**************************************************/

	// Transform変数を作る
	Transform transform { {1.0f,1.0f,1.0f},{0.0f,3.14f,0.0f},{0.0f,0.0f,0.0f} };

	// camera変数を作る
	Transform cameraTransform { {1.0f, 1.0f, 1.0f},{0.0f, 0.0f, 0.0f},{0.0f,0.0f,-10.0f} };

	/**************************************************
	* TransformationMatrix用のResourceを作る
	**************************************************/

	// WVP用のリソースを作る。Matrix4x4 1つ分のサイズを用意する
	Microsoft::WRL::ComPtr<ID3D12Resource> wvpResource = dxCommon->CreateBufferResource(sizeof(TransformationMatrix));
	// データを書き込む
	TransformationMatrix *wvpData = nullptr;
	// 書き込むためのアドレスを取得
	wvpResource->Map(0, nullptr, reinterpret_cast<void **>(&wvpData));
	// 単位行列を書き込んでおく
	wvpData->WVP = MakeIdentity4x4();
	wvpData->World = MakeAffineMatrix(transform.scale, transform.rotate, transform.translate);

	/**************************************************
	* Material用のResourceを作る
	**************************************************/

	// マテリアル用のリソースを作る。今回はcolor1つ分のサイズを用意する
	Microsoft::WRL::ComPtr<ID3D12Resource> materialResource = dxCommon->CreateBufferResource(sizeof(Material));
	// マテリアルにデータを書き込む
	Material *materialData = nullptr;
	// 書き込むためのアドレスを取得
	materialResource->Map(0, nullptr, reinterpret_cast<void **>(&materialData));
	// 今回は赤を書き込んでみる
	materialData->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
	materialData->enableLighting = false;
	materialData->uvTransform = MakeIdentity4x4();

	/**************************************************
	* ランバート反射
	**************************************************/

	// マテリアルリソースを作る
	Microsoft::WRL::ComPtr<ID3D12Resource> resourceDirectionalLight = dxCommon->CreateBufferResource(sizeof(DirectionalLight));

	// directionalLight用のリソースを作る
	DirectionalLight *directionalLightData = nullptr;

	resourceDirectionalLight->Map(0, nullptr, reinterpret_cast<void **>(&directionalLightData));

	// デフォルト値
	directionalLightData->color = { 1.0f,1.0f,1.0f,1.0f };
	directionalLightData->direction = { 0.0f,-1.0f,0.0f };
	directionalLightData->intensity = 1.0f;

#pragma endregion

#pragma region Resources: 3D Object (Triangle)

	/**************************************************
	* 頂点データの作成とビュー
	**************************************************/

	//----VertexResourceを生成する----

	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResourceTriangle = dxCommon->CreateBufferResource(sizeof(VertexData) * 3);

	//----VertexBufferViewを生成する----

	// 頂点バッファビューを作成する
	D3D12_VERTEX_BUFFER_VIEW vertexBufferViewTriangle {};
	// リソースの先頭のアドレスから使う
	vertexBufferViewTriangle.BufferLocation = vertexResourceTriangle->GetGPUVirtualAddress();
	// 使用するリソースのサイズは頂点6つ分のサイズ
	vertexBufferViewTriangle.SizeInBytes = sizeof(VertexData) * 3;
	// 1頂点あたりのサイズ
	vertexBufferViewTriangle.StrideInBytes = sizeof(VertexData);

	//----Resourceにデータを書き込む----

	// 頂点リソースにデータを書き込む
	VertexData *vertexDataTriangle = nullptr;
	// 書き込むためのアドレスを取得
	vertexResourceTriangle->Map(0, nullptr, reinterpret_cast<void **>(&vertexDataTriangle));
	// 左下
	vertexDataTriangle[0].position = { -0.5f,-0.5f,0.0f,1.0f };
	vertexDataTriangle[0].texcoord = { 0.0f,1.0f };
	vertexDataTriangle[0].normal = { 0.0f, 0.0f,-1.0f };
	// 上
	vertexDataTriangle[1].position = { 0.0f,0.5f,0.0f,1.0f };
	vertexDataTriangle[1].texcoord = { 0.5f,0.0f };
	vertexDataTriangle[1].normal = { 0.0f, 0.0f,-1.0f };
	// 右下
	vertexDataTriangle[2].position = { 0.5f,-0.5f,0.0f,1.0f };
	vertexDataTriangle[2].texcoord = { 1.0f,1.0f };
	vertexDataTriangle[2].normal = { 0.0f, 0.0f,-1.0f };

	/**************************************************
	* IndexResource
	**************************************************/

	Microsoft::WRL::ComPtr<ID3D12Resource> indexResourceTriangle = dxCommon->CreateBufferResource(sizeof(uint32_t) * 3);

	D3D12_INDEX_BUFFER_VIEW indexBufferViewTriangle {};
	indexBufferViewTriangle.BufferLocation = indexResourceTriangle->GetGPUVirtualAddress();
	indexBufferViewTriangle.SizeInBytes = sizeof(uint32_t) * 3;
	indexBufferViewTriangle.Format = DXGI_FORMAT_R32_UINT;

	uint32_t *indexDataTriangle = nullptr;
	indexResourceTriangle->Map(0, nullptr, reinterpret_cast<void **>(&indexDataTriangle));
	indexDataTriangle[0] = 0;
	indexDataTriangle[1] = 1;
	indexDataTriangle[2] = 2;

	/**************************************************
	* Transform
	**************************************************/

	// Transform変数を作る
	Transform transformTriangle { {1.0f,1.0f,1.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };

	// camera変数を作る
	Transform cameraTransformTriangle { {1.0f, 1.0f, 1.0f},{0.0f, 0.0f, 0.0f},{0.0f,0.0f,-5.0f} };

	/**************************************************
	* TransformationMatrix用のResourceを作る
	**************************************************/

	// WVP用のリソースを作る。Matrix4x4 1つ分のサイズを用意する
	Microsoft::WRL::ComPtr<ID3D12Resource> wvpResourceTriangle = dxCommon->CreateBufferResource(sizeof(TransformationMatrix));
	// データを書き込む
	TransformationMatrix *wvpDataTriangle = nullptr;
	// 書き込むためのアドレスを取得
	wvpResourceTriangle->Map(0, nullptr, reinterpret_cast<void **>(&wvpDataTriangle));
	// 単位行列を書き込んでおく
	wvpDataTriangle->WVP = MakeIdentity4x4();
	wvpDataTriangle->World = MakeAffineMatrix(transformTriangle.scale, transformTriangle.rotate, transformTriangle.translate);

	/**************************************************
	* Material用のResourceを作る
	**************************************************/

	// マテリアル用のリソースを作る。今回はcolor1つ分のサイズを用意する
	Microsoft::WRL::ComPtr<ID3D12Resource> materialResourceTriangle = dxCommon->CreateBufferResource(sizeof(Material));
	// マテリアルにデータを書き込む
	Material *materialDataTriangle = nullptr;
	// 書き込むためのアドレスを取得
	materialResourceTriangle->Map(0, nullptr, reinterpret_cast<void **>(&materialDataTriangle));
	// 今回は赤を書き込んでみる
	materialDataTriangle->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
	materialDataTriangle->enableLighting = false;
	materialDataTriangle->uvTransform = MakeIdentity4x4();

	/**************************************************
	* ランバート反射
	**************************************************/

	// マテリアルリソースを作る
	Microsoft::WRL::ComPtr<ID3D12Resource> resourceDirectionalLightTriangle = dxCommon->CreateBufferResource(sizeof(DirectionalLight));

	// directionalLight用のリソースを作る
	DirectionalLight *directionalLightDataTriangle = nullptr;

	resourceDirectionalLightTriangle->Map(0, nullptr, reinterpret_cast<void **>(&directionalLightDataTriangle));

	// デフォルト値
	directionalLightDataTriangle->color = { 1.0f,1.0f,1.0f,1.0f };
	directionalLightDataTriangle->direction = { 0.0f,-1.0f,0.0f };
	directionalLightDataTriangle->intensity = 1.0f;

#pragma endregion

#pragma region Resources: 3D Object (Sphere)

	// 球の分割数と頂点数計算
	uint32_t kSubdivision = 16;
	uint32_t vertexCount = (kSubdivision + 1) * (kSubdivision + 1);
	uint32_t indexCount = kSubdivision * kSubdivision * 6;

	/**************************************************
	* 頂点データの作成とビュー
	**************************************************/

	// 頂点リソースの作成
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResourceSphere = dxCommon->CreateBufferResource(sizeof(VertexData) * vertexCount);

	// 頂点バッファビューの作成
	D3D12_VERTEX_BUFFER_VIEW vertexBufferViewSphere {};
	vertexBufferViewSphere.BufferLocation = vertexResourceSphere->GetGPUVirtualAddress();
	vertexBufferViewSphere.SizeInBytes = sizeof(VertexData) * vertexCount;
	vertexBufferViewSphere.StrideInBytes = sizeof(VertexData);

	// 頂点データのマップと生成
	VertexData *vertexDataSphere = nullptr;
	vertexResourceSphere->Map(0, nullptr, reinterpret_cast<void **>(&vertexDataSphere));

	const float kLonEvery = std::numbers::pi_v<float> *2.0f / float(kSubdivision);
	const float kLatEvery = std::numbers::pi_v<float> / float(kSubdivision);

	for (uint32_t lat = 0; lat <= kSubdivision; ++lat) {
		float latAngle = -std::numbers::pi_v<float> / 2.0f + lat * kLatEvery;
		for (uint32_t lon = 0; lon <= kSubdivision; ++lon) {
			float lonAngle = lon * kLonEvery;

			uint32_t index = lat * (kSubdivision + 1) + lon;

			Vector3 pos = {
				std::cosf(latAngle) * std::cosf(lonAngle),
				std::sinf(latAngle),
				std::cosf(latAngle) * std::sinf(lonAngle)
			};

			vertexDataSphere[index] = {
				{ pos.x, pos.y, pos.z, 1.0f },
				{ float(lon) / kSubdivision, 1.0f - float(lat) / kSubdivision },
				{ pos.x, pos.y, pos.z }
			};
		}
	}

	/**************************************************
	* IndexResource
	**************************************************/

	Microsoft::WRL::ComPtr<ID3D12Resource> indexResourceSphere = dxCommon->CreateBufferResource(sizeof(uint32_t) * indexCount);

	D3D12_INDEX_BUFFER_VIEW indexBufferViewSphere {};
	indexBufferViewSphere.BufferLocation = indexResourceSphere->GetGPUVirtualAddress();
	indexBufferViewSphere.SizeInBytes = sizeof(uint32_t) * indexCount;
	indexBufferViewSphere.Format = DXGI_FORMAT_R32_UINT;

	uint32_t *indexDataSphere = nullptr;
	indexResourceSphere->Map(0, nullptr, reinterpret_cast<void **>(&indexDataSphere));

	uint32_t index = 0;
	for (uint32_t lat = 0; lat < kSubdivision; ++lat) {
		for (uint32_t lon = 0; lon < kSubdivision; ++lon) {

			uint32_t a = lat * (kSubdivision + 1) + lon;
			uint32_t b = (lat + 1) * (kSubdivision + 1) + lon;
			uint32_t c = lat * (kSubdivision + 1) + (lon + 1);
			uint32_t d = (lat + 1) * (kSubdivision + 1) + (lon + 1);

			// 三角形1: A-B-C
			indexDataSphere[index++] = a;
			indexDataSphere[index++] = b;
			indexDataSphere[index++] = c;

			// 三角形2: C-B-D
			indexDataSphere[index++] = c;
			indexDataSphere[index++] = b;
			indexDataSphere[index++] = d;
		}
	}

	/**************************************************
	* Transform
	**************************************************/

	Transform transformSphereInit;

	// SphereTransform変数を作る
	Transform transformSphere { {1.0f,1.0f,1.0f},{0.0f,-1.6f,0.0f},{0.0f,0.0f,0.0f} };
	transformSphereInit = transformSphere;

	// SphereCamera変数を作る
	Transform cameraTransformSphere { {1.0f, 1.0f, 1.0f},{0.0f, 0.0f, 0.0f},{0.0f,0.0f,-10.0f} };

	/**************************************************
	* TransformationMatrix用のResourceを作る
	**************************************************/

	// 1つ分のサイズを用意する
	Microsoft::WRL::ComPtr<ID3D12Resource> wvpResourceSphere = dxCommon->CreateBufferResource(sizeof(TransformationMatrix));
	// データを書き込む
	TransformationMatrix *wvpDataSphere = nullptr;
	// 書き込むためのアドレスを取得
	wvpResourceSphere->Map(0, nullptr, reinterpret_cast<void **>(&wvpDataSphere));
	// 単位行列を書き込んでおく
	wvpDataSphere->WVP = MakeIdentity4x4();
	wvpDataSphere->World = MakeAffineMatrix(transformSphere.scale, transformSphere.rotate, transformSphere.translate);

	/**************************************************
	* Material用のResourceを作る
	**************************************************/

	// マテリアル用のリソースを作る。今回はcolor1つ分のサイズを用意する
	Microsoft::WRL::ComPtr<ID3D12Resource> materialResourceSphere = dxCommon->CreateBufferResource(sizeof(Material));
	Material *materialDataSphere = nullptr;
	materialResourceSphere->Map(0, nullptr, reinterpret_cast<void **>(&materialDataSphere));
	materialDataSphere->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
	materialDataSphere->enableLighting = false;
	materialDataSphere->uvTransform = MakeIdentity4x4();
	materialDataSphere->shininess = 8.0f;

	/**************************************************
	* ランバート反射
	**************************************************/

	// マテリアルリソースを作る
	Microsoft::WRL::ComPtr<ID3D12Resource> resourceDirectionalLightSphere = dxCommon->CreateBufferResource(sizeof(DirectionalLight));

	// directionalLight用のリソースを作る
	DirectionalLight *directionalLightDataSphere = nullptr;

	resourceDirectionalLightSphere->Map(0, nullptr, reinterpret_cast<void **>(&directionalLightDataSphere));

	// デフォルト値
	directionalLightDataSphere->color = { 1.0f,1.0f,1.0f,1.0f };
	directionalLightDataSphere->direction = { 0.0f,-1.0f,0.0f };
	directionalLightDataSphere->intensity = 1.0f;

	Microsoft::WRL::ComPtr<ID3D12Resource> cameraResource = dxCommon->CreateBufferResource(sizeof(CameraForGPU));
	CameraForGPU *cameraData = nullptr;
	cameraResource->Map(0, nullptr, reinterpret_cast<void **>(&cameraData));
	cameraData->worldPosition = cameraTransformSphere.translate;

#pragma endregion

#pragma region Resources: 2D Object (Sprite)

	/**************************************************
	* 頂点データの作成とビュー
	**************************************************/

	// ----VertexResourceを生成する----
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResourceSprite = dxCommon->CreateBufferResource(sizeof(VertexData) * 4);

	// ----VertexBufferViewを生成する----

	// 頂点バッファビューを作成する
	D3D12_VERTEX_BUFFER_VIEW vertexBufferViewSprite {};
	// リソースの先頭のアドレスから使う
	vertexBufferViewSprite.BufferLocation = vertexResourceSprite->GetGPUVirtualAddress();
	// 使用するリソースのサイズは頂点6つ分のサイズ
	vertexBufferViewSprite.SizeInBytes = sizeof(VertexData) * 4;
	// 1頂点あたりのサイズ
	vertexBufferViewSprite.StrideInBytes = sizeof(VertexData);

	// ----Resourceにデータを書き込む----

	VertexData *vertexDataSprite = nullptr;
	vertexResourceSprite->Map(0, nullptr, reinterpret_cast<void **>(&vertexDataSprite));
	// 1枚目の三角形
	vertexDataSprite[0].position = { 0.0f,360.0f,0.0f,1.0f }; // 左下
	vertexDataSprite[0].texcoord = { 0.0f,1.0f };
	vertexDataSprite[0].normal = { 0.0f,0.0f,-1.0f };

	vertexDataSprite[1].position = { 0.0f,0.0f,0.0f,1.0f }; // 左上
	vertexDataSprite[1].texcoord = { 0.0f,0.0f };
	vertexDataSprite[1].normal = { 0.0f,0.0f,-1.0f };

	vertexDataSprite[2].position = { 640.0f,360.0f,0.0f,1.0f }; // 右下
	vertexDataSprite[2].texcoord = { 1.0f,1.0f };
	vertexDataSprite[2].normal = { 0.0f,0.0f,-1.0f };

	vertexDataSprite[3].position = { 640.0f,0.0f,0.0f,1.0f }; // 右上
	vertexDataSprite[3].texcoord = { 1.0f,0.0f };
	vertexDataSprite[3].normal = { 0.0f,0.0f,-1.0f };

	/**************************************************
	* IndexResource
	**************************************************/

	Microsoft::WRL::ComPtr<ID3D12Resource> indexResourceSprite = dxCommon->CreateBufferResource(sizeof(uint32_t) * 6);

	D3D12_INDEX_BUFFER_VIEW indexBufferViewSprite {};
	// リソースの先頭のアドレスから使う
	indexBufferViewSprite.BufferLocation = indexResourceSprite->GetGPUVirtualAddress();
	// 使用するリソースのサイズはインデックス6つ分のサイズ
	indexBufferViewSprite.SizeInBytes = sizeof(uint32_t) * 6;
	// インデックスはuint32_tとする
	indexBufferViewSprite.Format = DXGI_FORMAT_R32_UINT;

	// インデックスリソースにデータを書き込む
	uint32_t *indexDataSprite = nullptr;
	indexResourceSprite->Map(0, nullptr, reinterpret_cast<void **>(&indexDataSprite));
	indexDataSprite[0] = 0;
	indexDataSprite[1] = 1;
	indexDataSprite[2] = 2;
	indexDataSprite[3] = 1;
	indexDataSprite[4] = 3;
	indexDataSprite[5] = 2;

	/**************************************************
	* Transform
	**************************************************/

	// SpriteTransform変数を作る
	Transform transformSprite { {1.0f,1.0f,1.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };

	Transform uvTransformSprite { {1.0f,1.0f,1.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f} };

	/**************************************************
	* TransformationMatrix用のResourceを作る
	**************************************************/

	// Sprite用のTransformationMatrix用のリソースを作る。Matrix4×4 1つ分のサイズを用意する
	Microsoft::WRL::ComPtr<ID3D12Resource> wvpResourceSprite = dxCommon->CreateBufferResource(sizeof(TransformationMatrix));
	// データを書き込む
	TransformationMatrix *wvpDataSprite = nullptr;
	// 書き込むためのアドレスを取得
	wvpResourceSprite->Map(0, nullptr, reinterpret_cast<void **>(&wvpDataSprite));
	// 単位行列を書きこんでおく
	wvpDataSprite->WVP = MakeIdentity4x4();
	wvpDataSprite->World = MakeAffineMatrix(transformSprite.scale, transformSprite.rotate, transformSprite.translate);

	/**************************************************
	* Material用のResourceを作る
	**************************************************/

	// マテリアル用のリソースを作る。今回はcolor1つ分のサイズを用意する
	Microsoft::WRL::ComPtr<ID3D12Resource> materialResourceSprite = dxCommon->CreateBufferResource(sizeof(Material));
	Material *materialDataSprite = nullptr;
	materialResourceSprite->Map(0, nullptr, reinterpret_cast<void **>(&materialDataSprite));
	materialDataSprite->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
	// Lightingを有効にする
	materialDataSprite->enableLighting = false;
	materialDataSprite->uvTransform = MakeIdentity4x4();

#pragma endregion

#pragma region テクスチャを読み込んで使う

	// Textureを読んで転送する
	DirectX::ScratchImage mipImages = dxCommon->LoadTexture("resources/textures/monsterBall.png");
	const DirectX::TexMetadata &metadata = mipImages.GetMetadata();
	Microsoft::WRL::ComPtr<ID3D12Resource> textureResource = dxCommon->CreateTextureResource(metadata);
	Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource = dxCommon->UploadTextureData(textureResource, mipImages);

	// 2枚目のTextureを読んで転送する
	DirectX::ScratchImage mipImages2 = dxCommon->LoadTexture(modelData.material.textureFilePath);
	const DirectX::TexMetadata &metadata2 = mipImages2.GetMetadata();
	Microsoft::WRL::ComPtr<ID3D12Resource> textureResource2 = dxCommon->CreateTextureResource(metadata2);
	Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource2 = dxCommon->UploadTextureData(textureResource2, mipImages2);

#pragma endregion

#pragma region ShaderResourceViewを作る

	// metaDataを基にSRVの設定
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
	srvDesc.Format = metadata.format;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; // 2Dテクスチャ
	srvDesc.Texture2D.MipLevels = UINT(metadata.mipLevels);

	// SRVを作成するDescriptorHeapの場所を決める
	D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU = dxCommon->GetSRVCPUDescriptorHandle(1);
	D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU = dxCommon->GetSRVGPUDescriptorHandle(1);
	// SRVの生成
	dxCommon->GetDevice()->CreateShaderResourceView(textureResource.Get(), &srvDesc, textureSrvHandleCPU);

	// 2枚目のmetaDataを基にSRVの設定
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2 {};
	srvDesc2.Format = metadata2.format;
	srvDesc2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc2.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; // 2Dテクスチャ
	srvDesc2.Texture2D.MipLevels = UINT(metadata2.mipLevels);

	// 2枚目のSRVを作成するDescriptorHeapの場所を決める
	D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU2 = dxCommon->GetSRVCPUDescriptorHandle(2);
	D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU2 = dxCommon->GetSRVGPUDescriptorHandle(2);
	// 2枚目のSRVの生成
	dxCommon->GetDevice()->CreateShaderResourceView(textureResource2.Get(), &srvDesc2, textureSrvHandleCPU2);

#pragma endregion

#pragma region 状態管理（フラグなど）

	//平行光源の切り替え	
	bool useLighting = false;
	bool useLightingTriangle = false;
	bool useLightingSphere = false;

	bool showModelData = false;
	bool showTriangle = false;
	bool showSphere = true;
	bool showSprite = false;

#pragma endregion

#pragma region メインループ

	// ウィンドウの×ボタンが押されるまでループ
	while (true) {

		// Windowsのメッセージ処理
		if (winApp->ProcessMessage()) {
			// ゲームループを抜ける
			break;
		}

		// ゲームの処理

		// 入力の更新
		input->Update();

		if (input->PushKey(DIK_W))
		{
			transform.translate.y += 0.01f;
		}

		if (input->PushKey(DIK_S))
		{
			transform.translate.y -= 0.01f;
		}

		if (input->PushKey(DIK_A))
		{
			transform.translate.x -= 0.01f;
		}

		if (input->PushKey(DIK_D))
		{
			transform.translate.x += 0.01f;
		}

		if (input->PushKey(DIK_Q))
		{
			transform.rotate.y -= 0.01f;
		}

		if (input->PushKey(DIK_E))
		{
			transform.rotate.y += 0.01f;
		}

	#ifdef USE_IMGUI
		// フレームの開始
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
	#endif

	#ifdef USE_IMGUI
		ImGui::Begin("Settings");

		// 対象選択
		static int current = 1;
		const char *targets[] = { "ModelData","Sphere", "Sprite","Triangle" };
		ImGui::Combo("Target", &current, targets, IM_ARRAYSIZE(targets));

		ImGui::Separator();

		if (current == 0) {

			ImGui::Checkbox("Show ModelData", &showModelData);

			ImGui::Separator();
			ImGui::InputFloat3("CameraTranslate", &cameraTransform.translate.x);
			ImGui::SliderAngle("CameraRotateX", &cameraTransform.rotate.x);
			ImGui::SliderAngle("CameraRotateY", &cameraTransform.rotate.y);
			ImGui::SliderAngle("CameraRotateZ", &cameraTransform.rotate.z);
			ImGui::SliderAngle("RotateX", &transform.rotate.x);
			ImGui::SliderAngle("RotateY", &transform.rotate.y);
			ImGui::SliderAngle("RotateZ", &transform.rotate.z);
			ImGui::Separator();
			ImGui::ColorEdit3("Color", &materialData->color.x);
			ImGui::Separator();
			ImGui::Checkbox("enableLighting", &useLighting);
			useLighting ? materialData->enableLighting = true : materialData->enableLighting = false;
			ImGui::Separator();
			ImGui::ColorEdit3("Light Color", &directionalLightData->color.x);
			ImGui::SliderFloat3("Light Direction", &directionalLightData->direction.x, -1.0f, 1.0f);
			ImGui::SliderFloat("Light Intensity", &directionalLightData->intensity, 0.0f, 5.0f);

		} else if (current == 1) {

			ImGui::Checkbox("Show Sphere", &showSphere);

			// ===== Sphere =====
			ImGui::Text("Transform");
			ImGui::SliderFloat3("Scale", &transformSphere.scale.x, 0.1f, 5.0f);
			ImGui::SliderFloat3("Rotate", &transformSphere.rotate.x, -3.14f, 3.14f);
			ImGui::SliderFloat3("Translate", &transformSphere.translate.x, -5.0f, 5.0f);
			if (ImGui::Button("Reset transformSphere")) {
				transformSphere = transformSphereInit;
			}

			// ===== Material =====
			ImGui::Separator();
			ImGui::Text("Material");
			ImGui::ColorEdit3("Color", &materialDataSphere->color.x);

			ImGui::Separator();
			ImGui::Text("Camera");
			ImGui::SliderFloat3("Camera Position", &cameraTransformSphere.translate.x, -20.0f, 20.0f);

			ImGui::Separator();
			ImGui::Text("Directional Light");
			ImGui::ColorEdit3("Light Color", &directionalLightDataSphere->color.x);
			ImGui::SliderFloat3("Light Direction", &directionalLightDataSphere->direction.x, -1.0f, 1.0f);
			ImGui::SliderFloat("Light Intensity", &directionalLightDataSphere->intensity, 0.0f, 5.0f);

			ImGui::Separator();
			ImGui::Text("CheckBox");
			ImGui::Checkbox("enableLighting", &useLightingSphere);
			useLightingSphere ? materialDataSphere->enableLighting = true : materialDataSphere->enableLighting = false;
		} else if (current == 2) {

			ImGui::Checkbox("Show Sprite", &showSprite);

			// ===== Sprite =====
			ImGui::Text("Transform");
			ImGui::SliderFloat3("Scale", &transformSprite.scale.x, 0.1f, 5.0f);
			ImGui::SliderFloat3("Rotate", &transformSprite.rotate.x, -3.14f, 3.14f);
			ImGui::SliderFloat3("Translate", &transformSprite.translate.x, -640.0f, 640.0f);

			// ===== Material =====
			ImGui::Separator();
			ImGui::Text("Material");
			ImGui::ColorEdit3("Color", &materialDataSprite->color.x);

			ImGui::Separator();
			ImGui::Text("UVTransform");
			ImGui::DragFloat2("UVTranslate", &uvTransformSprite.translate.x, 0.01f, -10.0f, 10.0f);
			ImGui::DragFloat2("UVScale", &uvTransformSprite.scale.x, 0.01f, -10.0f, 10.0f);
			ImGui::SliderAngle("UVRotate", &uvTransformSprite.rotate.z);
		} else {

			ImGui::Checkbox("Show Triangle", &showTriangle);

			// ===== Triangle =====
			ImGui::Text("Transform");
			ImGui::SliderFloat3("Scale", &transformTriangle.scale.x, 0.1f, 5.0f);
			ImGui::SliderFloat3("Rotate", &transformTriangle.rotate.x, -3.14f, 3.14f);
			ImGui::SliderFloat3("Translate", &transformTriangle.translate.x, -5.0f, 5.0f);

			// ===== Material =====
			ImGui::Separator();
			ImGui::Text("Material");
			ImGui::ColorEdit3("Color", &materialDataTriangle->color.x);

			ImGui::Separator();
			ImGui::Text("Camera");
			ImGui::SliderFloat3("Camera Position", &cameraTransformTriangle.translate.x, -10.0f, 10.0f);

			ImGui::Separator();
			ImGui::Text("Directional Light");
			ImGui::ColorEdit3("Light Color", &directionalLightDataTriangle->color.x);
			ImGui::SliderFloat3("Light Direction", &directionalLightDataTriangle->direction.x, -1.0f, 1.0f);
			ImGui::SliderFloat("Light Intensity", &directionalLightDataTriangle->intensity, 0.0f, 5.0f);

			ImGui::Separator();
			ImGui::Text("CheckBox");
			ImGui::Checkbox("enableLighting", &useLightingTriangle);
			useLightingTriangle ? materialDataTriangle->enableLighting = true : materialDataTriangle->enableLighting = false;
		}

		ImGui::End();
	#endif

	#pragma region ワールド・ビュー・プロジェクション行列計算: 3D Object (ModelData)

		//transform.rotate.y += 0.01f;
		Matrix4x4 worldMatrix = MakeAffineMatrix(transform.scale, transform.rotate, transform.translate);
		Matrix4x4 cameraMatrix = MakeAffineMatrix(cameraTransform.scale, cameraTransform.rotate, cameraTransform.translate);
		Matrix4x4 viewMatrix = Inverse(cameraMatrix);
		Matrix4x4 projectionMatrix = MakePerspectiveFovMatrix(0.45f, float(WinApp::kClientWidth) / float(WinApp::kClientHeight), 0.1f, 100.0f);
		Matrix4x4 worldViewProjectionMatrix = Multiply(worldMatrix, Multiply(viewMatrix, projectionMatrix));
		wvpData->WVP = worldViewProjectionMatrix;
		wvpData->World = worldMatrix;

	#pragma endregion

	#pragma region ワールド・ビュー・プロジェクション行列計算: 3D Object (Triangle)

		//transform.rotate.y += 0.01f;
		Matrix4x4 worldMatrixTriangle = MakeAffineMatrix(transformTriangle.scale, transformTriangle.rotate, transformTriangle.translate);
		Matrix4x4 cameraMatrixTriangle = MakeAffineMatrix(cameraTransformTriangle.scale, cameraTransformTriangle.rotate, cameraTransformTriangle.translate);
		Matrix4x4 viewMatrixTriangle = Inverse(cameraMatrixTriangle);
		Matrix4x4 projectionMatrixTriangle = MakePerspectiveFovMatrix(0.45f, float(WinApp::kClientWidth) / float(WinApp::kClientHeight), 0.1f, 100.0f);
		Matrix4x4 worldViewProjectionMatrixTriangle = Multiply(worldMatrixTriangle, Multiply(viewMatrixTriangle, projectionMatrixTriangle));
		wvpDataTriangle->WVP = worldViewProjectionMatrixTriangle;
		wvpDataTriangle->World = worldMatrixTriangle;

	#pragma endregion

	#pragma region ワールド・ビュー・プロジェクション行列計算: 3D Object (Sphere)

		//transformSphere.rotate.y += 0.01f;
		Matrix4x4 worldMatrixSphere = MakeAffineMatrix(transformSphere.scale, transformSphere.rotate, transformSphere.translate);
		Matrix4x4 cameraMatrixSphere = MakeAffineMatrix(cameraTransformSphere.scale, cameraTransformSphere.rotate, cameraTransformSphere.translate);
		Matrix4x4 viewMatrixSphere = Inverse(cameraMatrixSphere);
		Matrix4x4 projectionMatrixSphere = MakePerspectiveFovMatrix(0.45f, float(WinApp::kClientWidth) / float(WinApp::kClientHeight), 0.1f, 100.0f);
		Matrix4x4 worldViewProjectionMatrixSphere = Multiply(worldMatrixSphere, Multiply(viewMatrixSphere, projectionMatrixSphere));
		wvpDataSphere->WVP = worldViewProjectionMatrixSphere;
		wvpDataSphere->World = worldMatrixSphere;

	#pragma endregion

	#pragma region ワールド・ビュー・プロジェクション行列計算: 2D Object (Sprite)	

		// Sprite用のWorldViewProjectionMatrixを作る
		Matrix4x4 worldMatrixSprite = MakeAffineMatrix(transformSprite.scale, transformSprite.rotate, transformSprite.translate);
		Matrix4x4 viewMatrixSprite = MakeIdentity4x4();
		Matrix4x4 projectionMatrixSprite = MakeOrthographicMatrix(0.0f, 0.0f, float(WinApp::kClientWidth), float(WinApp::kClientHeight), 0.0f, 100.0f);
		Matrix4x4 worldViewProjectionMatrixSprite = Multiply(worldMatrixSprite, Multiply(viewMatrixSprite, projectionMatrixSprite));
		wvpDataSprite->WVP = worldViewProjectionMatrixSprite;
		wvpDataSprite->World = worldMatrixSprite;

		Matrix4x4 uvTransformMatrix = MakeScaleMatrix(uvTransformSprite.scale);
		uvTransformMatrix = Multiply(uvTransformMatrix, MakeRotateZMatrix(uvTransformSprite.rotate.z));
		uvTransformMatrix = Multiply(uvTransformMatrix, MakeTranslateMatrix(uvTransformSprite.translate));
		materialDataSprite->uvTransform = uvTransformMatrix;

	#pragma endregion

	#ifdef USE_IMGUI
		// ImGuiの内部コマンドを生成する
		ImGui::Render();
	#endif

	#pragma region 画面をクリアする

		// 描画前処理
		dxCommon->PreDraw();

		// RootSignatureを設定。PSOに設定しているけどベット設定が必要
		dxCommon->GetCommandList()->SetGraphicsRootSignature(rootSignature.Get());
		dxCommon->GetCommandList()->SetPipelineState(graphicsPipelineState.Get()); // PSOを設定

	#pragma region 描画: 3D Object (ModelData)

		if (showModelData) {

			dxCommon->GetCommandList()->IASetVertexBuffers(0, 1, &vertexBufferView); // VBVを設定
			// 形状を設定。PSOに設定しているものとはまた別。同じものを設定すると考えておけばいい
			dxCommon->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			// マテリアルCBufferの場所を設定
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(0, materialResource->GetGPUVirtualAddress());
			// wvp用のCBufferの場所を設定
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(1, wvpResource->GetGPUVirtualAddress());
			// SRVのDescriptorTableの先頭を設定。2はrootParameter[2]である。
			dxCommon->GetCommandList()->SetGraphicsRootDescriptorTable(2, textureSrvHandleGPU);
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(3, resourceDirectionalLight->GetGPUVirtualAddress());

			dxCommon->GetCommandList()->DrawInstanced(UINT(modelData.vertices.size()), 1, 0, 0);

		}

	#pragma endregion

	#pragma region 描画: 3D Object (Triangle)

		if (showTriangle) {

			dxCommon->GetCommandList()->IASetVertexBuffers(0, 1, &vertexBufferViewTriangle); // VBVを設定
			dxCommon->GetCommandList()->IASetIndexBuffer(&indexBufferViewTriangle); // IBVを設定

			// 形状を設定。PSOに設定しているものとはまた別。同じものを設定すると考えておけばいい
			dxCommon->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			// マテリアルCBufferの場所を設定
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(0, materialResourceTriangle->GetGPUVirtualAddress());
			// wvp用のCBufferの場所を設定
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(1, wvpResourceTriangle->GetGPUVirtualAddress());
			// SRVのDescriptorTableの先頭を設定。2はrootParameter[2]である。
			dxCommon->GetCommandList()->SetGraphicsRootDescriptorTable(2, textureSrvHandleGPU);
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(3, resourceDirectionalLightTriangle->GetGPUVirtualAddress());

			dxCommon->GetCommandList()->DrawIndexedInstanced(3, 1, 0, 0, 0);

		}

	#pragma endregion

	#pragma region 描画: 3D Object (Sphere)

		if (showSphere) {

			dxCommon->GetCommandList()->IASetVertexBuffers(0, 1, &vertexBufferViewSphere); // VBVを設定
			dxCommon->GetCommandList()->IASetIndexBuffer(&indexBufferViewSphere); // IBVを設定
			dxCommon->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(0, materialResourceSphere->GetGPUVirtualAddress());
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(1, wvpResourceSphere->GetGPUVirtualAddress());
			dxCommon->GetCommandList()->SetGraphicsRootDescriptorTable(2, textureSrvHandleGPU);
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(3, resourceDirectionalLightSphere->GetGPUVirtualAddress());
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(4, cameraResource->GetGPUVirtualAddress());

			dxCommon->GetCommandList()->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
		}

	#pragma endregion

	#pragma region 描画: 2D Object (Sprite)

		if (showSprite) {

			// Spriteの描画。変更が必要なものだけ変更する
			dxCommon->GetCommandList()->IASetVertexBuffers(0, 1, &vertexBufferViewSprite); // VBVを設定
			dxCommon->GetCommandList()->IASetIndexBuffer(&indexBufferViewSprite); // IBVを設定
			dxCommon->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(0, materialResourceSprite->GetGPUVirtualAddress());
			// TransformationMatrixCBufferの場所を設定
			dxCommon->GetCommandList()->SetGraphicsRootConstantBufferView(1, wvpResourceSprite->GetGPUVirtualAddress());
			dxCommon->GetCommandList()->SetGraphicsRootDescriptorTable(2, textureSrvHandleGPU);
			//描画!(DrawCall/ドローコール)6個のインデックスを使用し1つのインスタンスを描画。その他は当面0で良い
			dxCommon->GetCommandList()->DrawIndexedInstanced(6, 1, 0, 0, 0);
		}

	#pragma endregion

	#ifdef USE_IMGUI
		// 実際のcommandListのImGuiの描画コマンドを積む
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dxCommon->GetCommandList());
	#endif

		// 描画後処理
		dxCommon->PostDraw();

	#pragma endregion
	}

#pragma endregion

#pragma region Object解放

	// WindowsAPIの終了処理
	winApp->Finalize();

#ifdef USE_IMGUI
	// ImGuiの終了処理。詳細はさして重要ではないので解説は省略する。
	// こういうもんである。初期化と逆順に行う
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
#endif

	//CloseHandle(fenceEvent);

	// 入力解放
	delete input;
	input = nullptr;

	// XAudio2解放
	xAudio2.Reset();
	// 音声データ解放
	SoundUnload(&soundData1);

	// WindowsAPI解放
	delete winApp;
	winApp = nullptr;

	// DirectX解放
	delete dxCommon;
	dxCommon = nullptr;

#pragma endregion

	return 0;
}