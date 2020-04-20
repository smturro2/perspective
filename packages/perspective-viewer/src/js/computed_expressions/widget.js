/******************************************************************************
 *
 * Copyright (c) 2017, the Perspective Authors.
 *
 * This file is part of the Perspective library, distributed under the terms of
 * the Apache License 2.0.  The full license can be found in the LICENSE file.
 *
 */

import {bindTemplate, throttlePromise} from "../utils.js";

import template from "../../html/computed_expression_widget.html";

import style from "../../less/computed_expression_widget.less";

import {ComputedExpressionColumnLexer, ColumnNameTokenType, FunctionTokenType, OperatorTokenType} from "./lexer";

import {expression_to_computed_column_config} from "./visitor";

import {tokenMatcher} from "chevrotain";
import {get_autocomplete_suggestions, extract_partial_function} from "./autocomplete.js";

// Eslint complains here because we don't do anything, but actually we globally
// register this class as a CustomElement
@bindTemplate(template, style) // eslint-disable-next-line no-unused-vars
class ComputedExpressionWidget extends HTMLElement {
    constructor() {
        super();

        this._parsed_expression = undefined;
        this.expressions = [];
        this._valid = false;
    }

    connectedCallback() {
        this._register_ids();
        this._register_callbacks();
        this._expression_editor.set_renderer(this.render_expression.bind(this));
        this._editor_observer = new MutationObserver(this._resize_editor.bind(this));
    }

    /**
     * Observe the editor when the widget is opened.
     */
    _observe_editor() {
        this._editor_observer.observe(this._expression_editor, {
            attributes: true,
            attributeFilter: ["style"]
        });

        // Focus on the editor immediately
        this._expression_editor.focus();

        // Render the initial autocomplete - all functions + column names
        this._render_initial_autocomplete();
    }

    /**
     * Dispatch an event on editor resize to notify the side panel, and
     * disconnect the observer.
     */
    _resize_editor() {
        const event = new CustomEvent("perspective-computed-expression-resize");
        this.dispatchEvent(event);
        this._editor_observer.disconnect();
    }

    /**
     * A stub for the widget to have access to `perspective-viewer`'s _get_type
     * method. Replaced by a reference to the proper method when the widget is
     * opened inside `perspective-viewer`.
     *
     * @param {String} name a column name
     */
    _get_type(name) {
        throw new Error(`Cannot get column type for "${name}".`);
    }

    /**
     * Returns a list of objects from column names, suitable for rendering
     * in the autocomplete widget.
     */
    _get_column_names() {
        // label = what is shown in the autocomplete DOM
        // value = what the fragment in the editor will be replaced with
        return this._get_view_all_column_names().map(name => {
            return {
                label: name,
                value: `"${name}"`
            };
        });
    }

    /**
     * Given an expression string, render it into markup. Called only when the
     * expression is not an empty string.
     *
     * @param {String} expression
     */
    render_expression(expression) {
        this._clear_autocomplete();
        const lex_result = ComputedExpressionColumnLexer.tokenize(expression);

        if (lex_result.errors.length > 0) {
            return `<span class="psp-expression__errored">${expression}</span>`;
        }

        const output = [];
        const names = this._get_view_all_column_names();

        for (const token of lex_result.tokens) {
            let class_name = "fragment";
            let content = token.image;
            if (tokenMatcher(token, FunctionTokenType)) {
                class_name = "function";
            } else if (tokenMatcher(token, OperatorTokenType)) {
                class_name = "operator";
            } else if (tokenMatcher(token, ColumnNameTokenType)) {
                const column_name = token.payload;
                const exists = names.includes(column_name);

                if (!exists) {
                    class_name = "errored";
                } else {
                    class_name = `column_name ${this._get_type(column_name)}`;
                }
            }
            output.push(`<span class="psp-expression__${class_name}">${content}</span>`);
        }

        return output.join("");
    }

    render_error(expression, error) {
        this._set_error(error, this._error);
        return `<span class="psp-expression__errored">${expression}</span>`;
    }

    /**
     * Validate the expression after the
     * `perspective-expression-editor-rendered` has been fired. Fires on every
     * event, even when the expression is an empty string.
     * @param {*} ev
     */
    @throttlePromise
    async _validate_expression(ev) {
        this._clear_autocomplete();
        const expression = ev.detail.text;

        if (expression.length === 0) {
            this._render_initial_autocomplete();
            this._clear_error();
            return;
        }

        try {
            // Use this just for validation. On anything short of a massive
            // expression, this should have no performance impact as we
            // share an instance of the parser throughout the viewer.
            this._parsed_expression = expression_to_computed_column_config(expression);
        } catch (e) {
            // Show autocomplete OR error, but not both
            this._clear_error();
            this._disable_save_button();

            // Show the column name autocomplete if there is an open quote
            // or open parenthesis. FIXME: this is too eager and should try to
            // differentiate between open/close quotes, which needs lexer
            // information - maybe step through tokens and generate the
            // autocomplete that way?
            const name_fragments = expression.match(/(["'])[\s\w()]*?$/);
            const has_name_fragments = name_fragments && name_fragments.length > 0 && name_fragments[0] !== '" ';
            const show_column_names = has_name_fragments || expression[expression.length - 1] === "(";

            if (show_column_names) {
                let fragment = "";
                let suggestions = this._get_column_names();
                if (has_name_fragments) {
                    fragment = name_fragments[0].substring(1);
                    suggestions = suggestions.filter(name => name.label.toLowerCase().startsWith(fragment.toLowerCase()));
                }
                this.render_autocomplete(expression, suggestions, true);
                return;
            } else {
                const lex_result = ComputedExpressionColumnLexer.tokenize(expression);
                const suggestions = get_autocomplete_suggestions(expression, lex_result);
                if (suggestions.length > 0) {
                    // Show autocomplete and not error box
                    this.render_autocomplete(expression, suggestions);
                    return;
                }
            }

            // Expression is syntactically valid but unparsable
            const message = e.message ? e.message : JSON.stringify(e);
            this._set_error(message, this._error);
            return;
        }

        // Take the parsed expression and type check it on the viewer,
        // which will call `_type_check_expression()` with a computed_schema.
        const event = new CustomEvent("perspective-computed-expression-type-check", {
            detail: {
                parsed_expression: this._parsed_expression
            }
        });

        this.dispatchEvent(event);

        return;
    }

    @throttlePromise
    async _type_check_expression(computed_schema, expected_types) {
        const parsed = this._parsed_expression || [];
        const invalid = [];

        for (const column of parsed) {
            if (!computed_schema[column.column]) {
                invalid.push(column.column);
            }
        }

        if (invalid.length > 0) {
            let message = "TypeError:\n";
            for (const col of invalid) {
                message += `- \`${col}\` expected input column types ${expected_types[col].join("/")}\n`;
            }
            this._set_error(message, this._error);
        } else {
            this._clear_error();
            this._enable_save_button();
        }
    }

    _save_expression() {
        if (!this._valid || this._save_button.getAttribute("disabled")) {
            return;
        }
        const expression = this._expression_editor.get_text();
        const parsed_expression = this._parsed_expression || [];

        const event = new CustomEvent("perspective-computed-expression-save", {
            detail: {
                expression: expression,
                parsed_expression: parsed_expression
            }
        });

        this.dispatchEvent(event);

        this.expressions.push(expression);
    }

    // TODO: split out the autocomplete at some point
    // TODO: make sure it only shows columns of the correct type
    // TODO: separate column-name only autocomplete?
    render_autocomplete(expression, suggestions, is_column_name) {
        this._autocomplete_container.style.display = "block";
        for (const suggestion of suggestions) {
            const span = document.createElement("span");
            if (!suggestion.label) {
                continue;
            }
            span.textContent = suggestion.label;
            span.classList.add("psp-computed-expression-widget__autocomplete--item");
            if (is_column_name) {
                span.classList.add("psp-computed-expression-widget__autocomplete--column-name");
                span.classList.add(this._get_type(suggestion.label));
            }
            span.setAttribute("data-value", suggestion.value);
            this._autocomplete_container.appendChild(span);
        }
    }

    _clear_autocomplete() {
        this._autocomplete_container.style.display = "none";
        this._autocomplete_container.innerHTML = "";
    }

    _render_initial_autocomplete() {
        this._clear_autocomplete();
        const suggestions = get_autocomplete_suggestions("");
        if (suggestions.length > 0) {
            // Show autocomplete and not error box
            this.render_autocomplete("", suggestions);
            this.render_autocomplete("", this._get_column_names(), true);
        }
    }

    _autocomplete_value_click(ev) {
        if (ev.target && ev.target.matches("span.psp-computed-expression-widget__autocomplete--item")) {
            const old_value = this._expression_editor.get_text();
            const new_value = ev.target.getAttribute("data-value");
            if (!new_value) {
                return;
            }
            const last_input = extract_partial_function(old_value);

            if (last_input && last_input !== '"') {
                // replace the fragment with the full function/operator
                const final_value = old_value.substring(0, old_value.length - last_input.length) + new_value;
                this._expression_editor._edit_area.innerText = final_value;
            } else {
                // Check whether we are appending a column name
                const quote_index = old_value.lastIndexOf('"');
                if (quote_index > -1 && new_value.indexOf('"') != -1) {
                    const final_value = old_value.substring(0, quote_index) + new_value;
                    this._expression_editor._edit_area.innerText = final_value;
                } else {
                    // Append the autocomplete value
                    const space = old_value.length > 0 && old_value.indexOf('"') === -1 ? " " : "";
                    this._expression_editor._edit_area.innerText += `${space}${new_value}`;
                }
            }

            this._expression_editor._reset_selection();
            this._expression_editor.update_content();

            this._clear_autocomplete();
        }
    }

    // UI actions
    _clear_expression_editor() {
        this._expression_editor.clear_content();
    }

    _close_expression_widget() {
        this.style.display = "none";
        this._side_panel_actions.style.display = "flex";
        this._clear_error();
        this._disable_save_button();
        this._clear_expression_editor();
        // Disconnect the observer.
        this._editor_observer.disconnect();
    }

    /**
     * Given an error message, display it in the DOM and disable the
     * save button.
     *
     * @param {String} error An error message to be displayed.
     * @param {HTMLElement} target an `HTMLElement` that displays the `error`
     * message.
     */
    _set_error(error, target) {
        if (target) {
            target.innerText = error;
            target.style.display = "block";
            this._disable_save_button();
        }
    }

    _clear_error() {
        this._error.innerText = "";
        this._error.style.display = "none";
    }

    _disable_save_button() {
        this._save_button.setAttribute("disabled", true);
        this._valid = false;
    }

    _enable_save_button() {
        this._save_button.removeAttribute("disabled");
        this._valid = true;
    }

    _editor_keydown(ev) {
        if (ev.detail.key === "Enter") {
            // Save the expression when enter is typed.
            ev.detail.preventDefault();
            ev.detail.stopPropagation();
            this._save_expression();
        }
    }

    /**
     * Map DOM IDs to class properties.
     */
    _register_ids() {
        this._side_panel_actions = this.parentElement.querySelector("#side_panel__actions");
        this._close_button = this.shadowRoot.querySelector("#psp-computed-expression-widget-close");
        this._expression_editor = this.shadowRoot.querySelector("perspective-expression-editor");
        this._autocomplete_container = this.shadowRoot.querySelector(".psp-computed-expression-widget__autocomplete");
        this._error = this.shadowRoot.querySelector("#psp-computed-expression-widget-error");
        this._save_button = this.shadowRoot.querySelector("#psp-computed-expression-widget-button-save");
    }

    /**
     * Map callback functions to class properties.
     */
    _register_callbacks() {
        this._close_button.addEventListener("click", this._close_expression_widget.bind(this));
        this._expression_editor.addEventListener("perspective-expression-editor-rendered", this._validate_expression.bind(this));
        this._expression_editor.addEventListener("perspective-expression-editor-keydown", this._editor_keydown.bind(this));
        this._autocomplete_container.addEventListener("click", this._autocomplete_value_click.bind(this));
        this._save_button.addEventListener("click", this._save_expression.bind(this));
    }
}