/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CodeCache.h"

#include "BytecodeGenerator.h"
#include "DirectEvalExecutable.h"
#include "IndirectEvalExecutable.h"
#include "ModuleProgramExecutable.h"
#include "ProgramExecutable.h"
#include "VariableEnvironmentInlines.h"
#include <wtf/TZoneMallocInlines.h>

#if defined(WEBKIT_IOS6)
#include "VM.h"
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/Threading.h>
#endif

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CodeCache);

void CodeCacheMap::pruneSlowCase()
{
#if defined(WEBKIT_IOS6)
    // The largest single burst of growth would otherwise become a floor the
    // capacity can never fall below again, so one heavy page keeps its cost for
    // the rest of the session. Let the floor decay towards the current burst
    // instead of ratcheting up to the worst one ever seen.
    int64_t burst = std::max(m_size - m_sizeAtLastPrune, static_cast<int64_t>(0));
    m_minCapacity = std::max(burst, (m_minCapacity * 3) / 4);
#else
    m_minCapacity = std::max(m_size - m_sizeAtLastPrune, static_cast<int64_t>(0));
#endif
    m_sizeAtLastPrune = m_size;
    m_timeAtLastPrune = ApproximateTime::now();

    if (m_capacity < m_minCapacity)
        m_capacity = m_minCapacity;

    while (m_size > m_capacity || !canPruneQuickly()) {
        MapType::iterator it = m_map.begin();

        writeCodeBlock(it->key, it->value);

        m_size -= it->key.length();
        m_map.remove(it);
    }
}

static void generateUnlinkedCodeBlockForFunctions(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const SourceCode& parentSource, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error)
{
    auto generate = [&](UnlinkedFunctionExecutable* unlinkedExecutable, CodeSpecializationKind constructorKind) {
        if (constructorKind == CodeSpecializationKind::CodeForConstruct && SourceParseModeSet(SourceParseMode::AsyncArrowFunctionMode, SourceParseMode::AsyncMethodMode, SourceParseMode::AsyncFunctionMode).contains(unlinkedExecutable->parseMode()))
            return;

        SourceCode source = unlinkedExecutable->linkedSourceCode(parentSource);
        UnlinkedFunctionCodeBlock* unlinkedFunctionCodeBlock = unlinkedExecutable->unlinkedCodeBlockFor(vm, source, constructorKind, codeGenerationMode, error, unlinkedExecutable->parseMode());
        if (unlinkedFunctionCodeBlock)
            generateUnlinkedCodeBlockForFunctions(vm, unlinkedFunctionCodeBlock, source, codeGenerationMode, error);
    };

    // FIXME: We should also generate CodeBlocks for CodeForConstruct
    // https://bugs.webkit.org/show_bug.cgi?id=193823
    for (unsigned i = 0; i < unlinkedCodeBlock->numberOfFunctionDecls(); i++)
        generate(unlinkedCodeBlock->functionDecl(i), CodeSpecializationKind::CodeForCall);
    for (unsigned i = 0; i < unlinkedCodeBlock->numberOfFunctionExprs(); i++)
        generate(unlinkedCodeBlock->functionExpr(i), CodeSpecializationKind::CodeForCall);
}

template <class UnlinkedCodeBlockType, class ExecutableType = ScriptExecutable>
UnlinkedCodeBlockType* generateUnlinkedCodeBlockImpl(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, DerivedContextType derivedContextType, bool isArrowFunctionContext, const TDZEnvironment* variablesUnderTDZ = nullptr, const PrivateNameEnvironment* privateNameEnvironment = nullptr, ExecutableType* executable = nullptr)
{
    typedef typename CacheTypes<UnlinkedCodeBlockType>::RootNode RootNode;
    bool isInsideOrdinaryFunction = executable && executable->isInsideOrdinaryFunction();

    std::unique_ptr<RootNode> rootNode = parse<RootNode>(
        vm, source, Identifier(), ImplementationVisibility::Public, JSParserBuiltinMode::NotBuiltin, lexicallyScopedFeatures, scriptMode, CacheTypes<UnlinkedCodeBlockType>::parseMode, FunctionMode::None, SuperBinding::NotNeeded, error, ConstructorKind::None, derivedContextType, evalContextType, privateNameEnvironment, nullptr, isInsideOrdinaryFunction);

    if (!rootNode)
        return nullptr;

    unsigned lineCount = rootNode->lastLine() - rootNode->firstLine();
    unsigned startColumn = rootNode->startColumn() + 1;
    bool endColumnIsOnStartLine = !lineCount;
    unsigned unlinkedEndColumn = rootNode->endColumn();
    unsigned endColumn = unlinkedEndColumn + (endColumnIsOnStartLine ? startColumn : 1);
    if (executable)
        executable->recordParse(rootNode->features(), rootNode->lexicallyScopedFeatures(), rootNode->hasCapturedVariables(), rootNode->lastLine(), endColumn);

    NeedsClassFieldInitializer needsClassFieldInitializer = NeedsClassFieldInitializer::No;
    PrivateBrandRequirement privateBrandRequirement = PrivateBrandRequirement::None;
    if constexpr (std::is_same_v<ExecutableType, DirectEvalExecutable>) {
        needsClassFieldInitializer = executable->needsClassFieldInitializer();
        privateBrandRequirement = executable->privateBrandRequirement();
    }
    ExecutableInfo executableInfo(false, privateBrandRequirement, false, ConstructorKind::None, scriptMode, SuperBinding::NotNeeded, CacheTypes<UnlinkedCodeBlockType>::parseMode, derivedContextType, needsClassFieldInitializer, isArrowFunctionContext, false, evalContextType);

    UnlinkedCodeBlockType* unlinkedCodeBlock = UnlinkedCodeBlockType::create(vm, executableInfo, codeGenerationMode);
    unlinkedCodeBlock->recordParse(rootNode->features(), rootNode->lexicallyScopedFeatures(), rootNode->hasCapturedVariables(), lineCount, unlinkedEndColumn);
    if (!source.provider()->sourceURLDirective().isNull())
        unlinkedCodeBlock->setSourceURLDirective(source.provider()->sourceURLDirective());
    if (!source.provider()->sourceMappingURLDirective().isNull())
        unlinkedCodeBlock->setSourceMappingURLDirective(source.provider()->sourceMappingURLDirective());

    RefPtr<TDZEnvironmentLink> parentVariablesUnderTDZ;
    if (variablesUnderTDZ)
        parentVariablesUnderTDZ = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(*variablesUnderTDZ), nullptr);
    error = BytecodeGenerator::generate(vm, rootNode.get(), source, unlinkedCodeBlock, codeGenerationMode, parentVariablesUnderTDZ, nullptr, privateNameEnvironment);

    if (error.isValid())
        return nullptr;

    return unlinkedCodeBlock;
}

template <class UnlinkedCodeBlockType, class ExecutableType>
UnlinkedCodeBlockType* generateUnlinkedCodeBlock(VM& vm, ExecutableType* executable, const SourceCode& source, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, const TDZEnvironment* variablesUnderTDZ = nullptr, const PrivateNameEnvironment* privateNameEnvironment = nullptr)
{
    return generateUnlinkedCodeBlockImpl<UnlinkedCodeBlockType, ExecutableType>(vm, source, executable->lexicallyScopedFeatures(), scriptMode, codeGenerationMode, error, evalContextType, executable->derivedContextType(), executable->isArrowFunctionContext(), variablesUnderTDZ, privateNameEnvironment, executable);
}

UnlinkedEvalCodeBlock* generateUnlinkedCodeBlockForDirectEval(VM& vm, DirectEvalExecutable* executable, const SourceCode& source, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, const TDZEnvironment* variablesUnderTDZ, const PrivateNameEnvironment* privateNameEnvironment)
{
    return generateUnlinkedCodeBlock<UnlinkedEvalCodeBlock>(vm, executable, source, scriptMode, codeGenerationMode, error, evalContextType, variablesUnderTDZ, privateNameEnvironment);
}

template <class UnlinkedCodeBlockType>
    requires (!std::same_as<UnlinkedCodeBlockType, UnlinkedEvalCodeBlock>)
UnlinkedCodeBlockType* recursivelyGenerateUnlinkedCodeBlock(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType)
{
    bool isArrowFunctionContext = false;
    UnlinkedCodeBlockType* unlinkedCodeBlock = generateUnlinkedCodeBlockImpl<UnlinkedCodeBlockType>(vm, source, lexicallyScopedFeatures, scriptMode, codeGenerationMode, error, evalContextType, DerivedContextType::None, isArrowFunctionContext);
    if (!unlinkedCodeBlock)
        return nullptr;

    generateUnlinkedCodeBlockForFunctions(vm, unlinkedCodeBlock, source, codeGenerationMode, error);
    return unlinkedCodeBlock;
}

UnlinkedProgramCodeBlock* recursivelyGenerateUnlinkedCodeBlockForProgram(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType)
{
    return recursivelyGenerateUnlinkedCodeBlock<UnlinkedProgramCodeBlock>(vm, source, lexicallyScopedFeatures, scriptMode, codeGenerationMode, error, evalContextType);
}

UnlinkedModuleProgramCodeBlock* recursivelyGenerateUnlinkedCodeBlockForModuleProgram(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType)
{
    return recursivelyGenerateUnlinkedCodeBlock<UnlinkedModuleProgramCodeBlock>(vm, source, lexicallyScopedFeatures, scriptMode, codeGenerationMode, error, evalContextType);
}

#if defined(WEBKIT_IOS6)

// A third of the web thread's launch is JavaScriptCore turning the site's functions
// into bytecode, one at a time, at the moment each is first called. That bytecode is a
// pure function of the source text and the code generation mode, so it can be made
// somewhere else - but not in this VM. CommonVM.cpp calls apiLock().makeWebThreadAware(),
// which makes JSLock::lock() take the web lock as well, and the web thread holds the web
// lock for whole runloop turns; a second thread reaching into the page's VM would queue
// behind the page instead of running beside it, and _WebThreadLock() calls CRASH()
// outright when the caller is neither the main nor the web thread. So the pass shares
// nothing with the page: its own thread, its own VM, its own heap, its own atom string
// table, its own copy of the source text. The only thing that crosses back is a
// serialized blob, handed to the provider's bytecode cache on the thread that runs the
// page, where CodeCacheMap::fetchFromDisk picks it up the next time this program is
// compiled - under the same SourceCodeKey, which decodeCodeBlock re-checks before it
// hands anything back.

namespace {

struct AheadOfTimeBytecodeJob {
    RefPtr<SourceProvider> provider;
    RefPtr<WTF::RunLoop> runLoop;
    String source;
    String sourceURL;
    URL sourceOriginURL;
    TextPosition startPosition;
    int startOffset { 0 };
    int endOffset { 0 };
    int firstLine { 1 };
    int startColumn { 1 };
    unsigned sourceHash { 0 };
    SourceTaintedOrigin taintedness { SourceTaintedOrigin::Untainted };
    LexicallyScopedFeatures lexicallyScopedFeatures { NoLexicallyScopedFeatures };
    JSParserScriptMode scriptMode { JSParserScriptMode::Classic };
    OptionSet<CodeGenerationMode> codeGenerationMode;
};

class AheadOfTimeBytecodeThread {
    WTF_MAKE_NONCOPYABLE(AheadOfTimeBytecodeThread);
public:
    AheadOfTimeBytecodeThread() = default;

    bool enqueue(AheadOfTimeBytecodeJob&&);

private:
    void run();

    static constexpr unsigned maximumRememberedPrograms = 256;

    Lock m_lock;
    Condition m_jobAvailable;
    Deque<AheadOfTimeBytecodeJob> m_queue WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashSet<unsigned> m_alreadyQueued WTF_GUARDED_BY_LOCK(m_lock);
    RefPtr<Thread> m_thread WTF_GUARDED_BY_LOCK(m_lock);
};

bool AheadOfTimeBytecodeThread::enqueue(AheadOfTimeBytecodeJob&& job)
{
    // Zero is the hash set's empty value, so a provider that hashes to it cannot be
    // remembered and is refused rather than queued over and over.
    if (!job.sourceHash)
        return false;

    Locker locker { m_lock };

    if (m_queue.size() >= Options::aheadOfTimeBytecodeQueueLength())
        return false;

    if (m_alreadyQueued.size() >= maximumRememberedPrograms)
        m_alreadyQueued.clear();
    if (!m_alreadyQueued.add(job.sourceHash).isNewEntry)
        return false;

    if (!m_thread)
        m_thread = Thread::create("JSC AOT Bytecode"_s, [this] { run(); }, ThreadType::Compiler, Thread::QOS::Utility);

    m_queue.append(WTF::move(job));
    m_jobAvailable.notifyOne();
    return true;
}

void AheadOfTimeBytecodeThread::run()
{
    Ref<VM> vm = VM::create();

    for (;;) {
        AheadOfTimeBytecodeJob job;
        {
            Locker locker { m_lock };
            while (m_queue.isEmpty())
                m_jobAvailable.wait(m_lock);
            job = m_queue.takeFirst();
        }

        RefPtr<CachedBytecode> bytecode;
        {
            JSLockHolder locker(vm.get());

            SourceCode source(
                RefPtr<SourceProvider> { StringSourceProvider::create(job.source, SourceOrigin { job.sourceOriginURL }, String { job.sourceURL }, job.taintedness, job.startPosition) },
                job.startOffset, job.endOffset, job.firstLine, job.startColumn);

            ParserError error;
            UnlinkedProgramCodeBlock* unlinkedCodeBlock = recursivelyGenerateUnlinkedCodeBlockForProgram(vm.get(), source, job.lexicallyScopedFeatures, job.scriptMode, job.codeGenerationMode, error, EvalContextType::None);

            // error also reports any nested function that failed to generate, and that
            // is not a reason to throw the program away. A function with no bytecode in
            // the blob decodes into an executable whose code block slot is empty, and
            // UnlinkedFunctionExecutable::unlinkedCodeBlockFor falls straight through to
            // generating it, so the page sees exactly the error it would have seen. Only
            // a null program is fatal here.
            if (unlinkedCodeBlock) {
                SourceCodeKey key(
                    source, String(), SourceCodeType::ProgramType, job.lexicallyScopedFeatures, job.scriptMode,
                    DerivedContextType::None, EvalContextType::None, false, job.codeGenerationMode,
                    std::nullopt);
                bytecode = encodeCodeBlock(vm.get(), key, unlinkedCodeBlock);
            }
        }

        {
            // sourceProviderCacheMap keys on RefPtr<SourceProvider>, so without this the
            // private VM would hold every source it has ever been given for the life of
            // the process.
            JSLockHolder locker(vm.get());
            vm->clearSourceProviderCaches();
            vm->codeCache()->clear();
            vm->heap.collectNow(Sync, CollectionScope::Full);
        }
        job.source = String();

        // Dispatched whether or not there is a blob: the job may hold the last reference
        // to the provider, and ~CachedScriptSourceProvider talks to WebCore, so the final
        // deref has to happen back on the thread that runs the page.
        RefPtr<WTF::RunLoop> runLoop = WTF::move(job.runLoop);
        runLoop->dispatch([provider = WTF::move(job.provider), bytecode = WTF::move(bytecode)] {
            if (!bytecode)
                return;
            provider->cacheBytecode([&] { return bytecode; });
        });
    }
}

} // anonymous namespace

bool enqueueAheadOfTimeBytecodeGeneration(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode)
{
    if (!Options::useAheadOfTimeBytecode())
        return false;

    SourceProvider* provider = source.provider();
    if (!provider || provider->sourceType() != SourceProviderSourceType::Program)
        return false;

    // cacheBytecode() silently drops the blob when the provider keeps no cache, and this
    // pass is far too expensive to run on the chance that someone is listening.
    if (!provider->wantsBytecodeCache())
        return false;

    // The pass rebuilds the program from its own copy of the text, so its SourceCodeKey
    // only matches if the SourceCode covers the whole provider: the key's hash comes from
    // the provider, not from the range. A script that is a slice of a larger document
    // would need the whole document copied to hash the same, and those are not the ones
    // that cost seconds.
    StringView text = provider->source();
    if (source.startOffset() || static_cast<unsigned>(source.endOffset()) != text.length())
        return false;

    unsigned length = text.length();
    if (length < Options::aheadOfTimeBytecodeMinimumSourceLength() || length > Options::aheadOfTimeBytecodeMaximumSourceLength())
        return false;

    AheadOfTimeBytecodeJob job;
    job.provider = provider;
    job.runLoop = &vm.runLoop();
    job.source = text.toString().isolatedCopy();
    job.sourceURL = provider->sourceURL().isolatedCopy();
    job.sourceOriginURL = provider->sourceOrigin().url().isolatedCopy();
    job.startPosition = provider->startPosition();
    job.startOffset = source.startOffset();
    job.endOffset = source.endOffset();
    job.firstLine = source.firstLine().oneBasedInt();
    job.startColumn = source.startColumn().oneBasedInt();
    job.sourceHash = provider->hash();
    job.taintedness = provider->sourceTaintedOrigin();
    job.lexicallyScopedFeatures = lexicallyScopedFeatures;
    job.scriptMode = scriptMode;
    job.codeGenerationMode = codeGenerationMode;

    static NeverDestroyed<AheadOfTimeBytecodeThread> generator;
    return generator->enqueue(WTF::move(job));
}

#endif // defined(WEBKIT_IOS6)

template <class UnlinkedCodeBlockType, class ExecutableType>
UnlinkedCodeBlockType* CodeCache::getUnlinkedGlobalCodeBlock(VM& vm, ExecutableType* executable, const SourceCode& source, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType)
{
    DerivedContextType derivedContextType = executable->derivedContextType();
    bool isArrowFunctionContext = executable->isArrowFunctionContext();
    SourceCodeKey key(
        source, String(), CacheTypes<UnlinkedCodeBlockType>::codeType, executable->lexicallyScopedFeatures(), scriptMode,
        derivedContextType, evalContextType, isArrowFunctionContext, codeGenerationMode,
        std::nullopt);
    UnlinkedCodeBlockType* unlinkedCodeBlock = m_sourceCode.findCacheAndUpdateAge<UnlinkedCodeBlockType>(vm, key);
    if (unlinkedCodeBlock && Options::useCodeCache()) {
        unsigned lineCount = unlinkedCodeBlock->lineCount();
        unsigned startColumn = unlinkedCodeBlock->startColumn() + source.startColumn().oneBasedInt();
        bool endColumnIsOnStartLine = !lineCount;
        unsigned endColumn = unlinkedCodeBlock->endColumn() + (endColumnIsOnStartLine ? startColumn : 1);
        executable->recordParse(unlinkedCodeBlock->codeFeatures(), unlinkedCodeBlock->lexicallyScopedFeatures(), unlinkedCodeBlock->hasCapturedVariables(), source.firstLine().oneBasedInt() + lineCount, endColumn);
        if (unlinkedCodeBlock->sourceURLDirective())
            source.provider()->setSourceURLDirective(unlinkedCodeBlock->sourceURLDirective());
        if (unlinkedCodeBlock->sourceMappingURLDirective())
            source.provider()->setSourceMappingURLDirective(unlinkedCodeBlock->sourceMappingURLDirective());
        return unlinkedCodeBlock;
    }

    unlinkedCodeBlock = generateUnlinkedCodeBlock<UnlinkedCodeBlockType, ExecutableType>(vm, executable, source, scriptMode, codeGenerationMode, error, evalContextType);

    if (unlinkedCodeBlock && Options::useCodeCache()) {
        m_sourceCode.addCache(key, SourceCodeValue(vm, unlinkedCodeBlock, m_sourceCode.age()));

#if defined(WEBKIT_IOS6)
        // Reaching here means every cache said no and this thread just paid for a full
        // parse of the program, so this is the moment we know the source is worth
        // regenerating: it is real, it is big, and nobody has bytecode for it. What the
        // web thread has in hand is the top level only - the functions are still
        // uncompiled - so encoding it now would fill the provider's one blob slot with
        // the emptiest possible version of the program. Hand the whole source to the
        // background pass instead and let it produce the version with the functions in
        // it. recursivelyGenerateUnlinkedCodeBlockForProgram assumes a plain global
        // program, so anything with a derived context or an arrow function context stays
        // on the ordinary path.
        if constexpr (std::is_same_v<UnlinkedCodeBlockType, UnlinkedProgramCodeBlock>) {
            if (derivedContextType == DerivedContextType::None && !isArrowFunctionContext && evalContextType == EvalContextType::None) {
                if (enqueueAheadOfTimeBytecodeGeneration(vm, source, executable->lexicallyScopedFeatures(), scriptMode, codeGenerationMode))
                    return unlinkedCodeBlock;
            }
        }
#endif

        key.source().provider().cacheBytecode([&] {
            return encodeCodeBlock(vm, key, unlinkedCodeBlock);
        });
    }

    return unlinkedCodeBlock;
}

UnlinkedProgramCodeBlock* CodeCache::getUnlinkedProgramCodeBlock(VM& vm, ProgramExecutable* executable, const SourceCode& source, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error)
{
    return getUnlinkedGlobalCodeBlock<UnlinkedProgramCodeBlock>(vm, executable, source, JSParserScriptMode::Classic, codeGenerationMode, error, EvalContextType::None);
}

UnlinkedEvalCodeBlock* CodeCache::getUnlinkedEvalCodeBlock(VM& vm, IndirectEvalExecutable* executable, const SourceCode& source, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType)
{
    return getUnlinkedGlobalCodeBlock<UnlinkedEvalCodeBlock>(vm, executable, source, JSParserScriptMode::Classic, codeGenerationMode, error, evalContextType);
}

UnlinkedModuleProgramCodeBlock* CodeCache::getUnlinkedModuleProgramCodeBlock(VM& vm, ModuleProgramExecutable* executable, const SourceCode& source, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error)
{
    return getUnlinkedGlobalCodeBlock<UnlinkedModuleProgramCodeBlock>(vm, executable, source, JSParserScriptMode::Module, codeGenerationMode, error, EvalContextType::None);
}

UnlinkedFunctionExecutable* CodeCache::getUnlinkedGlobalFunctionExecutable(VM& vm, const Identifier& name, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, OptionSet<CodeGenerationMode> codeGenerationMode, std::optional<int> functionConstructorParametersEndPosition, ParserError& error)
{
    bool isArrowFunctionContext = false;
    SourceCodeKey key(
        source, name.string(), SourceCodeType::FunctionType,
        lexicallyScopedFeatures,
        JSParserScriptMode::Classic,
        DerivedContextType::None,
        EvalContextType::FunctionEvalContext,
        isArrowFunctionContext,
        codeGenerationMode,
        functionConstructorParametersEndPosition);
    UnlinkedFunctionExecutable* executable = m_sourceCode.findCacheAndUpdateAge<UnlinkedFunctionExecutable>(vm, key);
    if (executable && Options::useCodeCache()) {
        if (!executable->sourceURLDirective().isNull())
            source.provider()->setSourceURLDirective(executable->sourceURLDirective());
        if (!executable->sourceMappingURLDirective().isNull())
            source.provider()->setSourceMappingURLDirective(executable->sourceMappingURLDirective());
        return executable;
    }

    JSTextPosition positionBeforeLastNewline;
    std::unique_ptr<ProgramNode> program = parseFunctionForFunctionConstructor(vm, source, lexicallyScopedFeatures, error, &positionBeforeLastNewline, functionConstructorParametersEndPosition);
    if (!program) {
        RELEASE_ASSERT(error.isValid());
        return nullptr;
    }

    // This function assumes an input string that would result in a single function declaration.
    StatementNode* funcDecl = program->singleStatement();
    if (!funcDecl) [[unlikely]] {
        JSToken token;
        error = ParserError(ParserError::SyntaxError, ParserError::SyntaxErrorIrrecoverable, token, "Parser error"_s, -1);
        return nullptr;
    }
    ASSERT(funcDecl->isFuncDeclNode());

    FunctionMetadataNode* metadata = static_cast<FuncDeclNode*>(funcDecl)->metadata();
    ASSERT(metadata);
    if (!metadata)
        return nullptr;
    
    metadata->overrideName(name);
    metadata->setEndPosition(positionBeforeLastNewline);
    // The Function constructor only has access to global variables, so no variables will be under TDZ unless they're
    // in the global lexical environment, which we always TDZ check accesses from.
    ConstructAbility constructAbility = constructAbilityForParseMode(metadata->parseMode());
    UnlinkedFunctionExecutable* functionExecutable = UnlinkedFunctionExecutable::create(vm, source, metadata, UnlinkedNormalFunction, constructAbility, InlineAttribute::None, JSParserScriptMode::Classic, nullptr, std::nullopt, std::nullopt, DerivedContextType::None, EvalContextType::FunctionEvalContext, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None);

    if (!source.provider()->sourceURLDirective().isNull())
        functionExecutable->setSourceURLDirective(source.provider()->sourceURLDirective());
    if (!source.provider()->sourceMappingURLDirective().isNull())
        functionExecutable->setSourceMappingURLDirective(source.provider()->sourceMappingURLDirective());

    // We initially start with hasCapturedVariables = false.
    functionExecutable->recordParse(program->features(), metadata->lexicallyScopedFeatures(), /* hasCapturedVariables */ false);

    if (Options::useCodeCache())
        m_sourceCode.addCache(key, SourceCodeValue(vm, functionExecutable, m_sourceCode.age()));
    return functionExecutable;
}

void CodeCache::updateCache(const UnlinkedFunctionExecutable* executable, const SourceCode& parentSource, CodeSpecializationKind kind, const UnlinkedFunctionCodeBlock* codeBlock)
{
    parentSource.provider()->updateCache(executable, parentSource, kind, codeBlock);
}

void CodeCache::write()
{
    for (auto& it : m_sourceCode)
        writeCodeBlock(it.key, it.value);
}

void writeCodeBlock(const SourceCodeKey& key, const SourceCodeValue& value)
{
    UnlinkedCodeBlock* codeBlock = dynamicDowncast<UnlinkedCodeBlock>(value.cell.get());
    if (!codeBlock)
        return;

    key.source().provider().commitCachedBytecode();
}

static SourceCodeKey sourceCodeKeyForSerializedBytecode(VM&, const SourceCode& sourceCode, SourceCodeType codeType, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode)
{
    return SourceCodeKey(
        sourceCode, String(), codeType, lexicallyScopedFeatures, scriptMode,
        DerivedContextType::None, EvalContextType::None, false, codeGenerationMode,
        std::nullopt);
}

SourceCodeKey sourceCodeKeyForSerializedProgram(VM& vm, const SourceCode& sourceCode)
{
    JSParserScriptMode scriptMode = JSParserScriptMode::Classic;
    return sourceCodeKeyForSerializedBytecode(vm, sourceCode, SourceCodeType::ProgramType, NoLexicallyScopedFeatures, scriptMode, { });
}

SourceCodeKey sourceCodeKeyForSerializedModule(VM& vm, const SourceCode& sourceCode)
{
    JSParserScriptMode scriptMode = JSParserScriptMode::Module;
    return sourceCodeKeyForSerializedBytecode(vm, sourceCode, SourceCodeType::ModuleType, StrictModeLexicallyScopedFeature, scriptMode, { });
}

RefPtr<CachedBytecode> serializeBytecode(VM& vm, UnlinkedCodeBlock* codeBlock, const SourceCode& source, SourceCodeType codeType, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, FileSystem::FileHandle& fileHandle, BytecodeCacheError& error, OptionSet<CodeGenerationMode> codeGenerationMode)
{
    return encodeCodeBlock(vm, sourceCodeKeyForSerializedBytecode(vm, source, codeType, lexicallyScopedFeatures, scriptMode, codeGenerationMode), codeBlock, fileHandle, error);
}

}
