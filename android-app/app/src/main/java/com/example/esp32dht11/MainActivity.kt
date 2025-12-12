package com.example.esp32dht11

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.unit.dp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import java.io.File
import java.nio.ByteBuffer
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject

data class FrameItem(val run: String, val file: String, val size: Long)

data class UiState(
    val status: String = "Idle",
    val baseUrl: String = "http://192.168.4.1",
    val token: String = "",
    val frames: List<FrameItem> = emptyList(),
    val preview: Bitmap? = null,
    val isBusy: Boolean = false,
    val videoPath: String? = null
)

class MainActivity : ComponentActivity() {
    private val viewModel: HttpViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        viewModel.loadPrefs(this)
        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    val uiState by viewModel.state.collectAsState()
                    MainScreen(
                        state = uiState,
                        onSave = { viewModel.savePrefs(this) },
                        onRefresh = { viewModel.fetchFrames() },
                        onPreview = { viewModel.fetchImage(it) },
                        onExportRun = { viewModel.exportRunToVideo(it, this) },
                        onBaseUrlChange = { viewModel.updateBaseUrl(it) },
                        onTokenChange = { viewModel.updateToken(it) }
                    )
                }
            }
        }
    }
}

class HttpViewModel : ViewModel() {
    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state

    private val client = OkHttpClient.Builder()
        .callTimeout(15, TimeUnit.SECONDS)
        .build()

    fun updateBaseUrl(url: String) {
        _state.value = _state.value.copy(baseUrl = url)
    }

    fun updateToken(token: String) {
        _state.value = _state.value.copy(token = token)
    }

    fun loadPrefs(context: Context) {
        val prefs = context.getSharedPreferences("esp32cam", Context.MODE_PRIVATE)
        _state.value = _state.value.copy(
            baseUrl = prefs.getString("baseUrl", _state.value.baseUrl) ?: _state.value.baseUrl,
            token = prefs.getString("token", "") ?: ""
        )
    }

    fun savePrefs(context: Context) {
        val prefs = context.getSharedPreferences("esp32cam", Context.MODE_PRIVATE)
        prefs.edit()
            .putString("baseUrl", _state.value.baseUrl)
            .putString("token", _state.value.token)
            .apply()
        updateStatus("已保存设置")
    }

    private fun apiUrl(path: String): String = _state.value.baseUrl.trimEnd('/') + path

    fun fetchFrames(page: Int = 1, pageSize: Int = 100) {
        viewModelScope.launch(Dispatchers.IO) {
            updateBusy(true)
            try {
                val req = Request.Builder()
                    .url(apiUrl("/frames?page=$page&page_size=$pageSize"))
                    .addHeader("X-Auth-Token", _state.value.token)
                    .build()
                client.newCall(req).execute().use { resp ->
                    if (!resp.isSuccessful) throw IllegalStateException("HTTP ${resp.code}")
                    val body = resp.body?.string() ?: ""
                    val json = JSONObject(body)
                    val items = json.optJSONArray("items") ?: return@use
                    val list = mutableListOf<FrameItem>()
                    for (i in 0 until items.length()) {
                        val obj = items.getJSONObject(i)
                        list.add(
                            FrameItem(
                                run = obj.getString("run"),
                                file = obj.getString("file"),
                                size = obj.optLong("size", 0)
                            )
                        )
                    }
                    _state.value = _state.value.copy(frames = list, status = "列表更新成功")
                }
            } catch (e: Exception) {
                updateStatus("拉取列表失败: ${e.message}")
            } finally {
                updateBusy(false)
            }
        }
    }

    fun fetchImage(item: FrameItem) {
        viewModelScope.launch(Dispatchers.IO) {
            updateBusy(true)
            try {
                val url = apiUrl("/frames/file?run=${item.run}&file=${item.file}")
                val req = Request.Builder()
                    .url(url)
                    .addHeader("X-Auth-Token", _state.value.token)
                    .build()
                client.newCall(req).execute().use { resp ->
                    if (!resp.isSuccessful) throw IllegalStateException("HTTP ${resp.code}")
                    val bytes = resp.body?.bytes() ?: ByteArray(0)
                    val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                    _state.value = _state.value.copy(preview = bmp, status = "已加载 ${item.file}")
                }
            } catch (e: Exception) {
                updateStatus("加载图片失败: ${e.message}")
            } finally {
                updateBusy(false)
            }
        }
    }

    fun exportRunToVideo(run: String, context: Context, fps: Int = 10) {
        val items = _state.value.frames.filter { it.run == run }.sortedBy { it.file }
        if (items.isEmpty()) {
            updateStatus("该 run 没有帧")
            return
        }
        viewModelScope.launch(Dispatchers.IO) {
            updateBusy(true)
            try {
                val outputDir = File(context.cacheDir, "videos").apply { mkdirs() }
                val outFile = File(outputDir, "run_${run}_${System.currentTimeMillis()}.mp4")
                createVideoFromFrames(items, fps, outFile)
                _state.value = _state.value.copy(videoPath = outFile.absolutePath)
                updateStatus("视频已生成: ${outFile.absolutePath}")
            } catch (e: Exception) {
                updateStatus("生成视频失败: ${e.message}")
            } finally {
                updateBusy(false)
            }
        }
    }

    private fun createVideoFromFrames(frames: List<FrameItem>, fps: Int, outFile: File) {
        // Download first frame to determine size.
        val firstBytes = downloadImageBytes(frames.first())
        val firstBitmap = BitmapFactory.decodeByteArray(firstBytes, 0, firstBytes.size)
        val width = firstBitmap.width
        val height = firstBitmap.height

        val format = MediaFormat.createVideoFormat("video/avc", width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
            setInteger(MediaFormat.KEY_BIT_RATE, width * height * 4) // modest bitrate
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        }

        val codec = MediaCodec.createEncoderByType("video/avc")
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        codec.start()

        val muxer = MediaMuxer(outFile.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        var trackIndex = -1
        var muxerStarted = false
        var framePtsUs = 0L
        val frameDurationUs = 1_000_000L / fps

        fun drainCodec(endOfStream: Boolean) {
            if (endOfStream) codec.signalEndOfInputStream()
            val bufferInfo = MediaCodec.BufferInfo()
            while (true) {
                val outputIndex = codec.dequeueOutputBuffer(bufferInfo, 10_000)
                when {
                    outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER -> if (!endOfStream) return
                    outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        if (muxerStarted) throw IllegalStateException("Format changed twice")
                        trackIndex = muxer.addTrack(codec.outputFormat)
                        muxer.start()
                        muxerStarted = true
                    }
                    outputIndex >= 0 -> {
                        val encodedData = codec.getOutputBuffer(outputIndex) ?: throw RuntimeException("null buffer")
                        if (bufferInfo.size > 0) {
                          if (!muxerStarted) throw RuntimeException("muxer hasn't started")
                          encodedData.position(bufferInfo.offset)
                          encodedData.limit(bufferInfo.offset + bufferInfo.size)
                          muxer.writeSampleData(trackIndex, encodedData, bufferInfo)
                        }
                        codec.releaseOutputBuffer(outputIndex, false)
                        if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return
                    }
                }
            }
        }

        // Feed frames.
        for (frame in frames) {
            val bytes = downloadImageBytes(frame)
            val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
            val nv21 = bitmapToNV21(bmp, width, height)
            val inputIndex = codec.dequeueInputBuffer(10_000)
            if (inputIndex >= 0) {
                val inputBuffer: ByteBuffer = codec.getInputBuffer(inputIndex)!!
                inputBuffer.clear()
                inputBuffer.put(nv21)
                codec.queueInputBuffer(
                    inputIndex,
                    0,
                    nv21.size,
                    framePtsUs,
                    0
                )
                framePtsUs += frameDurationUs
            }
            drainCodec(false)
        }
        drainCodec(true)
        codec.stop()
        codec.release()
        if (muxerStarted) muxer.stop()
        muxer.release()
    }

    private fun bitmapToNV21(bitmap: Bitmap, width: Int, height: Int): ByteArray {
        val scaled = if (bitmap.width != width || bitmap.height != height) {
            Bitmap.createScaledBitmap(bitmap, width, height, true)
        } else {
            bitmap
        }
        val argb = IntArray(width * height)
        scaled.getPixels(argb, 0, width, 0, 0, width, height)
        val yuv = ByteArray(width * height * 3 / 2)
        var yIndex = 0
        var uvIndex = width * height

        for (j in 0 until height) {
            for (i in 0 until width) {
                val rgb = argb[j * width + i]
                val r = (rgb shr 16) and 0xFF
                val g = (rgb shr 8) and 0xFF
                val b = rgb and 0xFF
                val y = ((66 * r + 129 * g + 25 * b + 128) shr 8) + 16
                val u = ((-38 * r - 74 * g + 112 * b + 128) shr 8) + 128
                val v = ((112 * r - 94 * g - 18 * b + 128) shr 8) + 128
                yuv[yIndex++] = y.coerceIn(0, 255).toByte()
                if (j % 2 == 0 && i % 2 == 0) {
                    yuv[uvIndex++] = v.coerceIn(0, 255).toByte()
                    yuv[uvIndex++] = u.coerceIn(0, 255).toByte()
                }
            }
        }
        if (scaled != bitmap) scaled.recycle()
        return yuv
    }

    private fun downloadImageBytes(frame: FrameItem): ByteArray {
        val url = apiUrl("/frames/file?run=${frame.run}&file=${frame.file}")
        val req = Request.Builder()
            .url(url)
            .addHeader("X-Auth-Token", _state.value.token)
            .build()
        client.newCall(req).execute().use { resp ->
            if (!resp.isSuccessful) throw IllegalStateException("HTTP ${resp.code}")
            return resp.body?.bytes() ?: ByteArray(0)
        }
    }

    private fun updateBusy(busy: Boolean) {
        _state.value = _state.value.copy(isBusy = busy)
    }

    private fun updateStatus(msg: String) {
        _state.value = _state.value.copy(status = msg)
    }
}

@Composable
fun MainScreen(
    state: UiState,
    onSave: () -> Unit,
    onRefresh: () -> Unit,
    onPreview: (FrameItem) -> Unit,
    onExportRun: (String) -> Unit,
    onBaseUrlChange: (String) -> Unit,
    onTokenChange: (String) -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text("状态: ${state.status}")
        OutlinedTextField(
            value = state.baseUrl,
            onValueChange = onBaseUrlChange,
            label = { Text("设备地址 (含协议)") },
            modifier = Modifier.fillMaxWidth()
        )
        OutlinedTextField(
            value = state.token,
            onValueChange = onTokenChange,
            label = { Text("X-Auth-Token") },
            modifier = Modifier.fillMaxWidth()
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = onSave, enabled = !state.isBusy) { Text("保存设置") }
            Button(onClick = onRefresh, enabled = !state.isBusy) { Text("刷新列表") }
        }

        if (state.preview != null) {
            Image(
                bitmap = state.preview.asImageBitmap(),
                contentDescription = "preview",
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 8.dp)
            )
        }

        Text("帧列表(${state.frames.size})")
        LazyColumn(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            items(state.frames) { item ->
                Column {
                    Text("${item.run} / ${item.file} (${item.size} bytes)")
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = { onPreview(item) }, enabled = !state.isBusy) { Text("查看") }
                        Button(onClick = { onExportRun(item.run) }, enabled = !state.isBusy) { Text("导出该 run") }
                    }
                }
            }
        }

        state.videoPath?.let { Text("最近生成视频: $it") }
    }
}
