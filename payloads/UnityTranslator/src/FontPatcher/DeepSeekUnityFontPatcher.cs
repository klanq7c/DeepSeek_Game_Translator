using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using Mono.Cecil;

public static class DeepSeekUnityFontPatcher
{
	public static IEnumerable<string> TargetDLLs
	{
		get
		{
			yield return "UnityEngine.TextRenderingModule.dll";
		}
	}

	public static void Patch(AssemblyDefinition assembly)
	{
		if (assembly == null)
		{
			throw new ArgumentNullException(nameof(assembly));
		}

		TypeDefinition fontType = assembly.MainModule.Types.FirstOrDefault(type => type.FullName == "UnityEngine.Font");
		if (fontType == null)
		{
			Trace.WriteLine("[DeepSeek Font Patcher] UnityEngine.Font is absent; no compatibility declaration was added.");
			return;
		}

		bool hasPublicFactory = fontType.Methods.Any(method =>
			method.Name == "CreateDynamicFontFromOSFont" &&
			method.IsStatic &&
			method.Parameters.Count == 2 &&
			method.Parameters[0].ParameterType.FullName == "System.String" &&
			method.Parameters[1].ParameterType.FullName == "System.Int32");
		bool hasInternalFactory = fontType.Methods.Any(IsInternalDynamicFontFactory);
		if (hasPublicFactory || hasInternalFactory)
		{
			Trace.WriteLine("[DeepSeek Font Patcher] Unity dynamic OS-font factory already exists; no patch was needed.");
			return;
		}

		MethodDefinition method = new MethodDefinition(
			"Internal_CreateDynamicFont",
			MethodAttributes.Private | MethodAttributes.Static | MethodAttributes.HideBySig,
			assembly.MainModule.TypeSystem.Void)
		{
			ImplAttributes = MethodImplAttributes.InternalCall
		};
		method.Parameters.Add(new ParameterDefinition("font", ParameterAttributes.None, fontType));
		method.Parameters.Add(new ParameterDefinition(
			"fontNames",
			ParameterAttributes.None,
			new ArrayType(assembly.MainModule.TypeSystem.String)));
		method.Parameters.Add(new ParameterDefinition(
			"size",
			ParameterAttributes.None,
			assembly.MainModule.TypeSystem.Int32));
		fontType.Methods.Add(method);

		Trace.WriteLine("[DeepSeek Font Patcher] Restored UnityEngine.Font.Internal_CreateDynamicFont(Font,string[],int) for a stripped managed profile.");
	}

	private static bool IsInternalDynamicFontFactory(MethodDefinition method)
	{
		return method.Name == "Internal_CreateDynamicFont" &&
			method.IsStatic &&
			method.Parameters.Count == 3 &&
			method.Parameters[0].ParameterType.FullName == "UnityEngine.Font" &&
			method.Parameters[1].ParameterType.FullName == "System.String[]" &&
			method.Parameters[2].ParameterType.FullName == "System.Int32";
	}
}
