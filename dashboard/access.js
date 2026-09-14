const adminToken = localStorage.getItem("admin_token");
if (!adminToken) {
    window.location.href = "login.html";
}
// Adds the admin JWT to protected API requests
async function adminFetch(url, options = {}) {
    const headers = {
        ...(options.headers || {}),
        "Authorization": `Bearer ${adminToken}`
    };
    const response = await fetch(url, {
        ...options,
        headers: headers
    });
    if (response.status === 401) {
        localStorage.removeItem("admin_token");
        alert("Your admin session has expired. Please log in again.");
        window.location.href = "login.html";
        return null;
    }
    return response;
}

async function loadAccessAttempts() {
    const response = await adminFetch("/admin/access-attempts");
    if (!response) {
        return;
    }
    const data = await response.json();
    const accessBody = document.getElementById("access-body");
    accessBody.innerHTML = "";
    if (!data.success) {
        alert(`Failed to load access attempts: ${data.reason}`);
        return;
    }
    for (const attempt of data.attempts) {
        const row = document.createElement("tr");
        const name = attempt.first_name
            ? `${attempt.first_name} ${attempt.last_name}`
            : "Unknown";
        row.innerHTML = `
            <td>${attempt.attempt_id}</td>
            <td>${attempt.employee_id ?? "-"}</td>
            <td>${name}</td>
            <td>${formatDate(attempt.started_at)}</td>
            <td>${formatDate(attempt.finished_at)}</td>
            <td>${attempt.first_factor ?? "-"}</td>
            <td>${attempt.second_factor ?? "-"}</td>
            <td>${attempt.overall_result ?? "IN PROGRESS"}</td>
            <td>${attempt.failure_reason ?? "-"}</td>
        `;
        accessBody.appendChild(row);
    }
}

function formatDate(dateValue) {
    if (!dateValue) {
        return "-";
    }
    return new Date(dateValue).toLocaleString();
}

document.getElementById("access-search").addEventListener("input", function() {
    const searchValue = this.value.trim().toLowerCase();
    const rows = document.querySelectorAll("#access-body tr");
    for (const row of rows) {
        const employeeId = row.children[1].textContent.trim().toLowerCase();
        row.style.display = employeeId.includes(searchValue) ? "" : "none";
    }
});

loadAccessAttempts();