### SRV和UAV
在 Compute Shader 的语境下，**SRV** 全称 **Shader Resource View（着色器资源视图）**。

你可以把它理解为 GPU 中为显存数据（Buffer/Texture）提供的**“只读接口”**。它本身不存储数据，而是描述如何“看待”或“解释”底层的一块显存。

**UAV** 全称 **Unordered Access View（无序访问视图）**。

和 SRV 的“只读”相反，它是 GPU 编程中**支持随机读写**的接口，是 Compute Shader 能够“写数据”的核心机制。

    
- **与 UAV 的区别**：
    
    - **SRV = 只读**（Shader 中可以读，不能写）。
        
    - **UAV = 读写**（用于写入结果，是 Compute Shader 输出数据的主要方式）。
        

在 UE 中写 Compute Shader，参数声明会是这样：

- `SHADER_PARAMETER_SRV(Texture2D, InputTexture)` —— 可读的纹理
    
- `SHADER_PARAMETER_SRV(StructuredBuffer<FMyStruct>, InputBuffer)` —— 可读的结构化缓冲区

简单理解它的特点：

- **可读可写**：Shader 既能读取其中的数据，也能向任意位置写入结果。这是 Compute Shader 把计算结果传回 CPU 或给后续 Pass 使用的最主要方式。
    
- **无序访问**：这里的“无序”主要是指它支持**随机访问**，即线程可以写入 Buffer/纹理的任意位置，不保证顺序。正因如此，才需要原子操作（`InterlockedAdd` 等）来处理多线程写同一位置的情况。

在 UE 的 Compute Shader 开发中，你在 C++ 里通常这样声明一个可写的 UAV 参数：

- `SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutputTexture)` —— 可读写的纹理
    
- `SHADER_PARAMETER_UAV(RWStructuredBuffer<FMyStruct>, OutputBuffer)` —— 可读写的结构化缓冲区
    

对应的 HLSL 声明就是 `RWTexture2D`、`RWStructuredBuffer`、`RWBuffer` 等以 `RW` 开头的类型。

### **SV_DispatchThreadID**
~~~
uint3 DispatchThreadId : SV_DispatchThreadID 
~~~
表示当前线程在**整个 dispatch 范围内**的全局线程 ID。
如果你用 compute shader 画纹理，最常用的就是它：