(function() {
    var editor = document.getElementById('editor');
    var preview = document.getElementById('preview');
    if (!editor || !preview) return;

    function updatePreview() {
        preview.innerHTML = marked.parse(editor.value);
    }

    editor.addEventListener('input', updatePreview);
    updatePreview();
})();
