#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace myengine::editor
{
    class IEditorCommand
    {
    public:
        virtual ~IEditorCommand() = default;

        virtual bool Undo() = 0;
        virtual bool Redo() = 0;
        virtual const std::string& Label() const = 0;
    };

    class LambdaEditorCommand final : public IEditorCommand
    {
    public:
        using Operation = std::function<bool()>;

        LambdaEditorCommand(std::string label, Operation undoOperation, Operation redoOperation);

        bool Undo() override;
        bool Redo() override;
        const std::string& Label() const override;

    private:
        std::string label_;
        Operation undoOperation_;
        Operation redoOperation_;
    };

    class EditorCommandHistory
    {
    public:
        void Clear();
        bool CanUndo() const;
        bool CanRedo() const;
        bool Undo();
        bool Redo();
        void Push(std::unique_ptr<IEditorCommand> command);

    private:
        std::vector<std::unique_ptr<IEditorCommand>> undoStack_;
        std::vector<std::unique_ptr<IEditorCommand>> redoStack_;
    };
}