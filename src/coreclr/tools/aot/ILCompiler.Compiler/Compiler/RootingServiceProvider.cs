// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using ILCompiler.DependencyAnalysis;
using ILCompiler.DependencyAnalysisFramework;

using Internal.TypeSystem;

using Debug = System.Diagnostics.Debug;

namespace ILCompiler
{
    internal delegate void RootAdder(object o, string reason);

    internal sealed class RootingServiceProvider : IRootingServiceProvider
    {
        private readonly NodeFactory _factory;
        private readonly RootAdder _rootAdder;

        public RootingServiceProvider(NodeFactory factory, RootAdder rootAdder)
        {
            _factory = factory;
            _rootAdder = rootAdder;
        }

        public void AddCompilationRoot(MethodDesc method, string reason, string exportName = null, bool exportHidden = false)
        {
            MethodDesc canonMethod = method.GetCanonMethodTarget(CanonicalFormKind.Specific);
            IMethodNode methodEntryPoint = _factory.MethodEntrypoint(canonMethod);
            _rootAdder(methodEntryPoint, reason);

            if (exportName != null)
            {
                exportName = _factory.NameMangler.NodeMangler.ExternMethod(exportName, method);
                _factory.NodeAliases.Add(methodEntryPoint, (exportName, exportHidden));
            }

            if (canonMethod != method && method.HasInstantiation)
                _rootAdder(_factory.MethodGenericDictionary(method), reason);
        }

        public void AddCompilationRoot(TypeDesc type, string reason)
        {
            _rootAdder(_factory.MaximallyConstructableType(type), reason);
        }

        public void AddReflectionRoot(TypeDesc type, string reason)
        {
            TypeDesc lookedAtType = type;
            do
            {
                _factory.TypeSystemContext.EnsureLoadableType(lookedAtType);
                lookedAtType = (lookedAtType as MetadataType)?.ContainingType;
            }
            while (lookedAtType != null);

            _rootAdder(_factory.ReflectedType(type), reason);
        }

        public void AddReflectionRoot(MethodDesc method, string reason)
        {
            if (!_factory.MetadataManager.IsReflectionBlocked(method))
            {
                _factory.TypeSystemContext.EnsureLoadableMethod(method);
                _rootAdder(_factory.ReflectedMethod(method.GetCanonMethodTarget(CanonicalFormKind.Specific)), reason);
            }
        }

        public void AddReflectionRoot(FieldDesc field, string reason)
        {
            if (!_factory.MetadataManager.IsReflectionBlocked(field))
            {
                _factory.TypeSystemContext.EnsureLoadableType(field.OwningType);
                _factory.TypeSystemContext.EnsureLoadableType(field.FieldType);
                _rootAdder(_factory.ReflectedField(field), reason);
            }
        }

        public void AddCompilationRoot(object o, string reason)
        {
            Debug.Assert(o is IDependencyNode<NodeFactory>);
            _rootAdder(o, reason);
        }

        public void RootThreadStaticBaseForType(TypeDesc type, string reason)
        {
            Debug.Assert(!type.IsGenericDefinition);

            MetadataType metadataType = type as MetadataType;
            if (metadataType != null && metadataType.ThreadGcStaticFieldSize.AsInt > 0)
            {
                _rootAdder(_factory.TypeThreadStaticIndex(metadataType), reason);

                // Also explicitly root the non-gc base if we have a lazy cctor
                if (_factory.PreinitializationManager.HasLazyStaticConstructor(type))
                    _rootAdder(_factory.TypeNonGCStaticsSymbol(metadataType), reason);
            }
        }

        public void RootGCStaticBaseForType(TypeDesc type, string reason)
        {
            Debug.Assert(!type.IsGenericDefinition);

            MetadataType metadataType = type as MetadataType;
            if (metadataType != null && metadataType.GCStaticFieldSize.AsInt > 0)
            {
                _rootAdder(_factory.TypeGCStaticsSymbol(metadataType), reason);

                // Also explicitly root the non-gc base if we have a lazy cctor
                if (_factory.PreinitializationManager.HasLazyStaticConstructor(type))
                    _rootAdder(_factory.TypeNonGCStaticsSymbol(metadataType), reason);
            }
        }

        public void RootNonGCStaticBaseForType(TypeDesc type, string reason)
        {
            Debug.Assert(!type.IsGenericDefinition);

            MetadataType metadataType = type as MetadataType;
            if (metadataType != null && (metadataType.NonGCStaticFieldSize.AsInt > 0 || _factory.PreinitializationManager.HasLazyStaticConstructor(type)))
            {
                _rootAdder(_factory.TypeNonGCStaticsSymbol(metadataType), reason);
            }
        }

        public void RootModuleMetadata(ModuleDesc module, string reason)
        {
            // RootModuleMetadata is kind of a hack - this is pretty much only used to force include
            // type forwarders from assemblies metadata generator would normally not look at.
            // This will go away when the temporary RD.XML parser goes away.
            if (_factory.MetadataManager is UsageBasedMetadataManager mdManager)
            {
                // If we wouldn't generate metadata for the global module type, don't root the metadata at all.
                // Global module type always gets metadata and if we're not generating it, this is not the right
                // compilation unit (we're likely doing multifile).
                if (mdManager.CanGenerateMetadata(module.GetGlobalModuleType()))
                {
                    _rootAdder(_factory.ModuleMetadata(module), reason);
                }
            }
        }

        public void RootReadOnlyDataBlob(byte[] data, int alignment, string reason, string exportName, bool exportHidden)
        {
            var blob = _factory.ReadOnlyDataBlob("__readonlydata_" + exportName, data, alignment);
            _rootAdder(blob, reason);
            exportName = _factory.NameMangler.NodeMangler.ExternVariable(exportName);
            _factory.NodeAliases.Add(blob, (exportName, exportHidden));
        }

        public void RootDelegateMarshallingData(DefType type, string reason)
        {
            _rootAdder(_factory.DelegateMarshallingData(type), reason);
        }

        public void RootStructMarshallingData(DefType type, string reason)
        {
            _rootAdder(_factory.StructMarshallingData(type), reason);
        }
    }
}
