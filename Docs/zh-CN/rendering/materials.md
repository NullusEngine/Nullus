# 材质与 ShaderLab

材质是“着色器 + 参数 + 渲染状态”的组合。ShaderLab 是描述着色器与材质属性的源格式，材质则是引用着色器的资产。

## ShaderLab 结构

```hlsl
Shader "Nullus/StandardPBR"
{
    Properties
    {
        _BaseMap("Base Map", Texture2D) = "white"
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
        _Metallic("Metallic", Range(0, 1)) = 0
        _Roughness("Roughness", Range(0, 1)) = 1
    }

    SubShader
    {
        Tags
        {
            "RenderPipeline" = "Nullus"
            "RenderType" = "Opaque"
            "Queue" = "Geometry"
        }

        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            Cull Back
            ZWrite On
            ZTest LessEqual
            Blend Off
        }
    }
}
```

顶层包含四类信息：

| 块 | 作用 |
| --- | --- |
| `Shader "名字"` | 着色器的稳定名称与显示路径 |
| `Properties` | 材质暴露的属性与默认值 |
| `SubShader Tags` | 渲染管线、渲染类型与渲染队列 |
| `Pass` | 入口点、渲染状态与 HLSL 代码 |

## 属性类型

| 类型 | 默认值写法 | 用途 |
| --- | --- | --- |
| `Float` | `= 1` | 标量 |
| `Range(min, max)` | `= 0.5` | 带范围的标量，编辑器显示为滑块 |
| `Vector` | `= (1, 0, 0, 0)` | 四分量 |
| `Color` | `= (1, 1, 1, 1)` | 颜色，编辑器显示为颜色选择器 |
| `Texture2D` | `= "white"` | 二维纹理 |
| `TextureCube` | 立方体贴图名 | 立方体贴图 |
| `Int` | `= 0` | 整数 |

纹理默认值使用内置名（`white`、`black`、`bump`）而不是路径，保证材质在没有指定贴图时也有确定结果。

## Pass 渲染状态

| 语句 | 取值 | 说明 |
| --- | --- | --- |
| `Cull` | `Back` / `Front` / `Off` | 面剔除 |
| `ZWrite` | `On` / `Off` | 深度写入 |
| `ZTest` | `LessEqual` / 其他比较函数 | 深度比较 |
| `Blend` | `Off` / `SrcAlpha OneMinusSrcAlpha` 等 | 混合模式 |

透明物体关闭深度写入并使用 Alpha 混合；不透明物体开启深度写入并使用 `LessEqual`。

## 着色器变体

```hlsl
#pragma vertex VSMain
#pragma fragment PSMain
#pragma shader_feature _ALPHATEST_ON
#pragma multi_compile _ _NORMALMAP
#pragma multi_compile _ MAIN_LIGHT_SHADOWS
```

`shader_feature` 用于只有被材质实际引用的变体才会编译的关键字；`multi_compile` 用于需要完整排列的关键字。变体集合决定编译成本，渲染器只接受已编译的变体组合。

## 引擎自带着色器

| 着色器 | 用途 |
| --- | --- |
| `Nullus/StandardPBR` | 标准 PBR 不透明材质 |
| `Nullus/ShaderLab/Transparent` | Alpha 混合透明材质 |
| `Nullus/ShaderLab/UnlitColor` | 纯色无光照 |
| `Nullus/ShaderLab/UnlitTexture` | 纹理无光照 |
| `Nullus/ShaderLab/AlphaTest` | Alpha 裁剪 |
| `Nullus/ShaderLab/ForwardDepthOnly` | 深度预通道 |

延迟路径使用独立的 HLSL 着色器：`DeferredGBuffer`、`DeferredLighting`、`DeferredDecal`、`Skybox`、`HZBBuild`、`HZBOcclusion`。

## StandardPBR 参数

`Nullus/StandardPBR` 暴露的属性包括：

- `_BaseMap` / `_BaseColor`：基础色与基础贴图；
- `_Metallic` / `_Roughness` / `_AmbientOcclusion`：标量金属度、粗糙度与环境光遮蔽；
- `_MetallicMapChannel` / `_RoughnessMapChannel`：从打包贴图中取通道；
- `_MetallicMap` / `_RoughnessMap` / `_OcclusionMap` / `_NormalMap` / `_OpacityMap` / `_EmissiveMap` / `_SpecularMap`：各类贴图；
- `_EmissiveColor` / `_SpecularColor`：自发光与高光 tint；
- `_NormalScale`：法线强度；
- `_Cutoff`：Alpha 裁剪阈值。

金属度与粗糙度可以来自独立贴图，也可以从同一张贴图的不同通道读取，避免为每个通道单独存一张图。

## 共享着色器库

`NullusShaderLibrary` 提供可复用的 HLSL 头文件：

| 头文件 | 内容 |
| --- | --- |
| `Core.hlsl` | 基础类型与常用函数 |
| `Common.hlsl` | 通用工具 |
| `BRDF.hlsl` | 光照模型 |
| `Lighting.hlsl` | 光照计算 |
| `PBRNormals.hlsl` | 法线处理 |
| `StandardPBRSurface.hlsl` | StandardPBR 表面数据 |
| `Shadows.hlsl` | 阴影采样 |
| `SpaceTransforms.hlsl` | 坐标空间转换 |
| `Instancing.hlsl` | 实例化数据 |
| `Texture.hlsl` | 纹理采样封装 |
| `Fog.hlsl` | 雾效 |

新增着色器应优先复用这些头文件，而不是复制实现。

## 编辑器中的材质工作流

从 Asset Browser 打开 `.mat`，在 Material Editor 中编辑材质参数和源依赖。`.shader` 使用 ShaderLab/HLSL 编辑区，`.shadergraph` 提供 Nodes 与 Source JSON。Asset View 提供独立预览。

| 动作 | 结果 |
| --- | --- |
| 编辑参数或节点 | 更新 Working 文档并进入撤销历史；成功编译后可预览 |
| Save / Save source | 写入源文件，更新 Saved 状态 |
| `Compile` / `Compile / Preview` | 编译当前工作内容并检查诊断 |
| Apply to Scene | 把成功编译的工作快照应用到场景，更新 Applied 状态 |
| Save and Apply | 保存后应用；任一步失败都需要处理诊断 |
| Revert Working | 丢弃工作修改，恢复保存内容 |
| Publish resources | 对已应用材质发布资源产物 |

Save 与 Apply 是独立动作：保存不表示场景已换用该版本，预览成功也不表示已保存。编译失败时保留上一份有效场景结果。

参数来自目标变体的反射布局。ShaderGraph、ShaderLab、纹理和材质依赖随编译快照冻结，Cook 后 Player 消费已发布工件；不要用编辑器临时结果替代目标平台的编译和验证。

## ShaderLab 重载

重载着色器时先校验请求的变体，成功后更新关联材质并使受影响的管线代次失效。校验失败时保留旧状态，不会让场景处于半更新状态。

## 着色器约定

### 文件职责

| 后缀 | 内容 |
| --- | --- |
| `*.hlsl` | 具体着色器入口与 pass 局部逻辑 |
| `*.hlsli` | 共享 include、结构体、常量与 BRDF／数学工具 |

一个文件负责一组管线阶段组合或一个渲染 pass 的入口点。可复用的数学、材质采样、光照辅助与绑定声明放进 include。后端翻译产物不得手工编辑。

### 绑定空间

| 空间 | 内容 |
| --- | --- |
| `set0` | Frame / Scene |
| `set1` | Material |
| `set2` | Object |
| `set3` | Pass / Rare |

不要把低编号空间临时改作他用。属于材质的数据在所有后端都停在 Material 空间。

### 寄存器分配

为每个对外绑定的资源写下显式 HLSL 寄存器：

| 资源 | 约定 |
| --- | --- |
| 常量缓冲 | `b#` |
| 纹理 | `t#` |
| 结构化／只读缓冲 | `t#` 或 `StructuredBuffer` |
| 读写缓冲与纹理 | `u#` |
| 采样器 | `s#` |

```hlsl
cbuffer MaterialConstants : register(b0, space1)
{
    float4 g_BaseColor;
    float g_Metallic;
    float g_Roughness;
};

Texture2D g_BaseColorTex : register(t0, space1);
SamplerState g_LinearWrap : register(s0, space1);

cbuffer ObjectConstants : register(b0, space2)
{
    float4x4 g_Model;
};
```

规则：

- 同一空间内绑定索引必须稠密且确定；
- 不同空间的同一编号互不相关；
- 绑定顺序必须来自着色器声明或生成的元数据，不能来自渲染器代码里的临时调用顺序。

### 命名

| 对象 | 约定 | 示例 |
| --- | --- | --- |
| 类型 | PascalCase | `FrameConstants`、`MaterialConstants` |
| 函数 | PascalCase | `GetNormalWS`、`EvaluateCookTorrance` |
| 局部变量 | camelCase | `baseColor` |
| 全局资源与常量缓冲成员 | `g_` 前缀 | `g_BaseColor`、`g_ClusterLightIndices` |

阶段入口点使用显式名称：`VSMain`、`PSMain`、`CSMain`。同一文件存在多个变体时按用途加后缀，例如 `VSDepthOnly`、`PSDeferredLighting`、`CSClusterBuild`。

暴露给工具的材质参数优先使用语义名，而不是后端名。

### 常量缓冲规则

- 按更新频率分组：帧、pass、材质、对象；
- 不要把逐帧字段与逐对象字段混进同一个缓冲；
- 布局必须 16 字节对齐，并且在反射下保持稳定；
- 不要在手写 CPU 代码里假设后端特定的打包方式；
- CPU 上传结构体应由反射元数据生成或对其校验。

### 采样器规则

采样器必须显式声明，不要假设纹理对象自带采样状态。默认命名：`g_PointClamp`、`g_PointWrap`、`g_LinearClamp`、`g_LinearWrap`、`g_AnisoWrap`。

### Include 规则

共享 include 不应声明后端特定宏（例如 `#ifdef OPENGL`）；它可以提供数学、BRDF、打包辅助与共享参数结构体。一个 include 尽量只负责一个概念领域，例如 `CommonMath.hlsli`、`PBR.hlsli`、`MaterialBindings.hlsli`。

### 禁止的做法

- 同一个特性同时维护 HLSL 与手写 GLSL 两份源；
- 直接修改后端翻译产物；
- 在共享 include 里写后端特化分支；
- 用手工编号替代反射生成的绑定信息。

## 下一步

- [渲染概览](overview.md)：材质在帧中的位置。
- [模型、网格与纹理](../assets/models-textures.md)：贴图的导入与压缩。
